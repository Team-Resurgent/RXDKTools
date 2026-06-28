using System.Text;
using Rxdk.XbFile;
using Rxdk.Xbdm;
using Rxdk.Xbdm.Managed;

namespace Rxdk.XboxLaunch;

public sealed class XboxLaunchRunner : IDisposable
{
    private const int RebootTimeoutMs = 120_000;

    private readonly TextWriter _output;
    private readonly TextWriter _error;

    private XbConsoleSession? _session;
    private IXbdmDebugConnection? _debug;
    private IXbdmNotificationSession? _notifySession;

    private readonly ManualResetEventSlim _breakEvent = new(false);
    private readonly ManualResetEventSlim _execPendingEvent = new(false);

    private uint _mainThread;
    private int _execState = XbdmDebugConstants.DmnExecStart;
    private bool _awaitingTitleThread;
    private bool _seenTitleMod;
    private bool _launchStopped;
    private string _launchTitleBase = string.Empty;

    public XboxLaunchRunner(TextWriter? output = null, TextWriter? error = null)
    {
        _output = output ?? Console.Out;
        _error = error ?? Console.Error;
    }

    public XboxLaunchExitCode Run(XboxLaunchOptions options)
    {
        try
        {
            _session = XbConsoleSession.Connect(options.ConsoleName);
            _debug = _session.Connection.Debug;
            _debug.UseSharedConnection(true);

            OpenNotifySession();

            if (options.Reboot || _execState != XbdmDebugConstants.DmnExecPending)
            {
                EnsurePendingExec(options.TimeoutMs);
                CloseNotifySession();
                _debug.UseSharedConnection(false);
                _debug.UseSharedConnection(true);
                OpenNotifySession();
            }

            _output.WriteLine($"SetTitle dir={options.Directory} title={options.Title}");
            _debug.SetTitle(options.Directory!, options.Title!,
                string.IsNullOrEmpty(options.CommandLine) ? null : options.CommandLine);

            // Launch-and-run: do NOT set an initial breakpoint or stop-on-thread-
            // create. Just Go and stream the title's debug output for the window;
            // the title keeps running on the kit afterwards (matches a manual
            // launch). The stop-based path below is for handing off to a debugger.
            if (options.Go)
            {
                _awaitingTitleThread = false;
                _seenTitleMod = false;
                SetLaunchBaseFromTitle(options.Title!);

                _debug.Go();

                _output.WriteLine(
                    $"Launched (run mode); streaming debug output up to {options.TimeoutMs} ms (title runs free)...");
                // Notifications stream on the session callback thread; just keep
                // this thread alive for the window. Don't wait on _breakEvent --
                // a thread-create/break would cut streaming short.
                System.Threading.Thread.Sleep(options.TimeoutMs);
                _output.WriteLine("Launch window elapsed; title continues running on the kit.");
                return XboxLaunchExitCode.Success;
            }

            _debug.SetInitialBreakpoint();
            _debug.StopOn(XbdmDebugConstants.DmstopCreateThread, stop: true);

            _mainThread = 0;
            _awaitingTitleThread = true;
            _seenTitleMod = false;
            _launchStopped = false;
            SetLaunchBaseFromTitle(options.Title!);
            _breakEvent.Reset();

            _debug.Go();

            _output.WriteLine($"Waiting up to {options.TimeoutMs} ms for initial break...");
            if (!_breakEvent.Wait(options.TimeoutMs))
            {
                _error.WriteLine("Timed out waiting for initial stop (createthread/break).");
                return XboxLaunchExitCode.Error;
            }

            _awaitingTitleThread = false;

            if (_mainThread == 0)
            {
                _error.WriteLine("Title rebooted or crashed before main thread (returned to dashboard).");
                return XboxLaunchExitCode.Error;
            }

            if (!_launchStopped)
                _debug.Stop();

            _debug.ConnectDebugger(true);

            _output.WriteLine($"OK: stopped at entry (thread={_mainThread}). Debugger connected.");
            return XboxLaunchExitCode.Success;
        }
        catch (XbdmException ex) when (ex.HResultCode == XbdmHResults.NoXboxName)
        {
            _error.WriteLine("No Xbox console configured (XBDM_NOXBOX).");
            return XboxLaunchExitCode.NoConsole;
        }
        catch (XbdmException ex)
        {
            PrintHr("XBDM", ex);
            return XboxLaunchExitCode.Error;
        }
        catch (XbFileException ex)
        {
            _error.WriteLine(ex.Message);
            return XboxLaunchExitCode.NoConsole;
        }
        catch (XboxLaunchUsageException)
        {
            throw;
        }
        catch (Exception ex)
        {
            _error.WriteLine(ex.Message);
            return XboxLaunchExitCode.Error;
        }
        finally
        {
            CloseNotifySession();
            if (_debug is not null)
                _debug.UseSharedConnection(false);
        }
    }

    public void Dispose()
    {
        CloseNotifySession();
        _session?.Dispose();
        _breakEvent.Dispose();
        _execPendingEvent.Dispose();
    }

    private void OpenNotifySession()
    {
        _notifySession = _debug!.OpenNotificationSession(XbdmDebugConstants.DmPersistent);
        Register(XbdmDebugConstants.DmExec);
        Register(XbdmDebugConstants.DmCreateThread);
        Register(XbdmDebugConstants.DmBreak);
        Register(XbdmDebugConstants.DmModLoad);
        Register(XbdmDebugConstants.DmDebugStr);
        Register(XbdmDebugConstants.DmException);
        Register(XbdmDebugConstants.DmRip);
    }

    private void Register(int code) =>
        _notifySession!.Notify((uint)code, OnNotification);

    private void CloseNotifySession()
    {
        if (_notifySession is null)
            return;

        _notifySession.Dispose();
        _notifySession = null;
    }

    private void EnsurePendingExec(int timeoutMs)
    {
        if (_execState == XbdmDebugConstants.DmnExecPending)
            return;

        _execPendingEvent.Reset();
        _execState = XbdmDebugConstants.DmnExecReboot;
        _output.WriteLine("Rebooting Xbox to pending exec (STOP|WARM)...");
        _debug!.Reboot(XbdmDebugConstants.DmbootStop | XbdmDebugConstants.DmbootWarm);
        if (!_execPendingEvent.Wait(Math.Min(timeoutMs, RebootTimeoutMs)))
            throw new XbdmException("EnsurePendingExec timeout", -1);
        if (_execState != XbdmDebugConstants.DmnExecPending)
            throw new XbdmException("EnsurePendingExec", -1);
    }

    private void OnNotification(uint notification, object? data)
    {
        var code = (int)(notification & XbdmDebugConstants.NotificationMask);
        var stopped = (notification & XbdmDebugConstants.StopThread) != 0;
        _output.WriteLine($"notify: {NotifyName(code)}{(stopped ? " (stop)" : "")}");

        switch (code)
        {
            case XbdmDebugConstants.DmCreateThread when data is XbdmCreateThreadNotification created:
                if (_awaitingTitleThread && !_seenTitleMod)
                    break;
                if (_mainThread == 0)
                    _mainThread = created.ThreadId;
                _output.WriteLine($"  main thread id={created.ThreadId} start=0x{created.StartAddress:x}");
                if (stopped)
                    _launchStopped = true;
                if (_awaitingTitleThread && _seenTitleMod)
                    _breakEvent.Set();
                else if (!_awaitingTitleThread)
                    _breakEvent.Set();
                break;

            case XbdmDebugConstants.DmBreak when data is XbdmBreakNotification brk:
                _output.WriteLine($"  break addr=0x{brk.Address:x} thread={brk.ThreadId}");
                if (stopped)
                    _launchStopped = true;
                if (_awaitingTitleThread && _seenTitleMod)
                    _breakEvent.Set();
                else if (!_awaitingTitleThread)
                    _breakEvent.Set();
                break;

            case XbdmDebugConstants.DmModLoad when data is XbdmModLoadNotification mod:
                _output.WriteLine($"  module {mod.Name} base=0x{mod.BaseAddress:x} size={mod.Size}");
                if (_awaitingTitleThread && ModuleMatchesLaunchTitle(mod.Name))
                    _seenTitleMod = true;
                break;

            case XbdmDebugConstants.DmExec when data is int execState:
                _execState = execState;
                _output.WriteLine($"  exec state={execState}");
                if (execState == XbdmDebugConstants.DmnExecPending)
                    _execPendingEvent.Set();
                if (_awaitingTitleThread && _seenTitleMod && _mainThread == 0 &&
                    execState == XbdmDebugConstants.DmnExecReboot)
                    _breakEvent.Set();
                break;

            case XbdmDebugConstants.DmException when data is XbdmExceptionNotification ex:
                _output.WriteLine(
                    $"  exception code=0x{ex.Code:x8} thread={ex.ThreadId} addr=0x{ex.Address:x} flags=0x{ex.Flags:x}");
                break;

            case XbdmDebugConstants.DmRip:
                _output.WriteLine("  rip (title terminated)");
                if (_awaitingTitleThread && _seenTitleMod && _mainThread == 0)
                    _breakEvent.Set();
                break;

            case XbdmDebugConstants.DmDebugStr when data is XbdmDebugStringNotification dbg:
                _output.WriteLine($"  debugstr[{dbg.ThreadId}]: {dbg.Text}");
                break;

            default:
                _output.WriteLine($"  notification code={notification} param=0x{notification:x8}");
                break;
        }
    }

    private void SetLaunchBaseFromTitle(string title) =>
        _launchTitleBase = Path.GetFileNameWithoutExtension(title).ToLowerInvariant();

    private bool ModuleMatchesLaunchTitle(string name)
    {
        if (string.IsNullOrEmpty(_launchTitleBase))
            return false;
        var baseName = Path.GetFileName(name.Replace('/', '\\')).ToLowerInvariant();
        return baseName.Contains(_launchTitleBase, StringComparison.Ordinal);
    }

    private void PrintHr(string what, XbdmException ex)
    {
        var message = XbdmHResults.Describe(ex.HResultCode);
        _error.WriteLine($"{what} failed: {message} (0x{ex.HResultCode:x8})");
    }

    private static string NotifyName(int code) => code switch
    {
        XbdmDebugConstants.DmBreak => "DM_BREAK",
        XbdmDebugConstants.DmDebugStr => "DM_DEBUGSTR",
        XbdmDebugConstants.DmExec => "DM_EXEC",
        XbdmDebugConstants.DmSingleStep => "DM_SINGLESTEP",
        XbdmDebugConstants.DmModLoad => "DM_MODLOAD",
        XbdmDebugConstants.DmModUnload => "DM_MODUNLOAD",
        XbdmDebugConstants.DmCreateThread => "DM_CREATETHREAD",
        XbdmDebugConstants.DmDestroyThread => "DM_DESTROYTHREAD",
        XbdmDebugConstants.DmException => "DM_EXCEPTION",
        XbdmDebugConstants.DmAssert => "DM_ASSERT",
        XbdmDebugConstants.DmDataBreak => "DM_DATABREAK",
        XbdmDebugConstants.DmRip => "DM_RIP",
        XbdmDebugConstants.DmSectionLoad => "DM_SECTIONLOAD",
        XbdmDebugConstants.DmSectionUnload => "DM_SECTIONUNLOAD",
        XbdmDebugConstants.DmFiber => "DM_FIBER",
        _ => "DM_?",
    };
}

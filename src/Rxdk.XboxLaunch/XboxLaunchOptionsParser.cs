namespace Rxdk.XboxLaunch;

public static class XboxLaunchOptionsParser
{
    public static XboxLaunchOptions Parse(string[] args)
    {
        var options = new XboxLaunchOptions();

        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            if (arg is "/?" or "-?")
                throw new XboxLaunchUsageException();

            if (IsSwitch(arg, "dir") && i + 1 < args.Length)
            {
                options.Directory = args[++i];
                continue;
            }

            if (IsSwitch(arg, "title") && i + 1 < args.Length)
            {
                options.Title = args[++i];
                continue;
            }

            if (IsSwitch(arg, "cmd") && i + 1 < args.Length)
            {
                options.CommandLine = args[++i];
                continue;
            }

            if (IsSwitch(arg, "x") && i + 1 < args.Length)
            {
                options.ConsoleName = args[++i];
                continue;
            }

            if (IsSwitch(arg, "reboot"))
            {
                options.Reboot = true;
                continue;
            }

            if (IsSwitch(arg, "go"))
            {
                options.Go = true;
                continue;
            }

            if (IsSwitch(arg, "timeout") && i + 1 < args.Length)
            {
                options.TimeoutMs = int.Parse(args[++i]);
                continue;
            }

            throw new XboxLaunchUsageException();
        }

        if (string.IsNullOrWhiteSpace(options.Directory) || string.IsNullOrWhiteSpace(options.Title))
            throw new XboxLaunchUsageException();

        return options;
    }

    private static bool IsSwitch(string arg, string name)
    {
        if (arg.Length < 2)
            return false;

        if (arg[0] is not '/' and not '-')
            return false;

        return string.Equals(arg[1..], name, StringComparison.OrdinalIgnoreCase);
    }
}

public sealed class XboxLaunchUsageException : Exception
{
    public XboxLaunchUsageException() : base("Invalid command line.") { }
}

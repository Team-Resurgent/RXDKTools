struct Point { int x; int y; };
int add(int a, int b) { int sum = a + b; return sum; }
int main(void) {
    long hr = 0;
    int arr[4];
    struct Point p;
    p.x = 3; p.y = 7;
    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;
    hr = add(p.x, arr[0]);
    return (int)hr + p.y + arr[3];
}

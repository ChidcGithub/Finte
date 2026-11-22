/* Tiny shared object loaded dynamically by the kernel into a user process. */
int svc_add(int a, int b)
{
    return a + b;
}

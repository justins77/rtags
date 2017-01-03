#include <stdio.h>

#include <rct/Path.h>

void DoResolve(const char* path) {
    printf("orig: %s\n", path);
    {
        Path p = Path(path).resolved(Path::MakeAbsolute);
        printf("resolved absolute: %s\n", p.c_str());
        printf("resolved absolute canonicalized: %s\n", p.canonicalized().c_str());
    }
    {
        Path p = Path(path).resolved(Path::RealPath);
        printf("resolved real: %s\n", p.c_str());
    }
    printf("\n");
}


int main(int argc, char** argv) {
    DoResolve("/home/skydio/aircam");
    DoResolve("/home/skydio/aircam/../aircam");
}

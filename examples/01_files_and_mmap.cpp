// Files, locking, and mmap — the L0 layer.
// Build: cmake -DLLBT_BUILD_EXAMPLES=ON, run from anywhere.
#include <llbt/api.hpp>
#include <cstdio>
#include <cstring>

using namespace llbt::util;

int main()
{
    std::string path = File::resolve("llbt_example_file.bin", make_temp_dir());

    // Exclusive create + write, with an OS-level lock held.
    {
        File f(path, File::mode_Write);
        bool got_lock = f.try_rw_lock_exclusive();
        if (!got_lock) {
            std::fprintf(stderr, "file is locked by someone else\n");
            return 1;
        }
        f.resize(4096);
        File::Map<char> map(f, File::access_ReadWrite, 4096);
        std::strcpy(map.get_addr(), "hello from a memory-mapped page");
        map.sync(); // flush to disk
        f.rw_unlock();
    }

    // Reopen read-only and read through a fresh mapping.
    {
        File f(path, File::mode_Read);
        File::Map<char> map(f, File::access_ReadOnly, 4096);
        std::printf("read back: %s\n", map.get_addr());
    }

    File::remove(path);
    std::printf("ok\n");
    return 0;
}

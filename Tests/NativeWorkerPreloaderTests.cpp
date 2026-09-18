#include "../Source/NativeWorkerPreloader.h"

#include <windows.h>

#include <cassert>
#include <filesystem>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    assert(argc == 1);
    const auto worker = std::filesystem::absolute(argv[0]).parent_path()
        / L"NativeWorkerStub.exe";
    assert(std::filesystem::is_regular_file(worker));

    {
        hybrid::NativeWorkerPreloader preloader(
            worker, L"unused-vl.vxd", worker, L"unused-sg.vxd", 44'100,
            true, true);
        preloader.start();
        preloader.start();
        Sleep(250);
        for (std::uint8_t voice = 0;
             voice < hybrid::NativeWorkerPreloader::vlVoiceCount; ++voice) {
            const auto result = preloader.takeVl(voice);
            assert(result.client != nullptr);
            assert(result.failure.empty());
        }
        const auto sg = preloader.takeSg();
        assert(sg.client != nullptr);
        assert(sg.failure.empty());
        preloader.stop();
        preloader.stop();
    }

    {
        hybrid::NativeWorkerPreloader preloader(
            worker.parent_path() / L"missing-worker.exe", L"unused-vl.vxd",
            {}, {}, 44'100, true, false);
        preloader.start();
        Sleep(50);
        const auto failed = preloader.takeVl(0);
        assert(failed.client == nullptr);
        assert(!failed.failure.empty());
    }

    {
        hybrid::NativeWorkerPreloader preloader(
            worker, L"unused-vl.vxd", worker, L"unused-sg.vxd", 44'100,
            true, true);
        const auto claimed = preloader.takeVl(7);
        assert(claimed.client == nullptr);
        assert(claimed.failure.empty());
        preloader.start();
        preloader.stop();
    }

    for (int iteration = 0; iteration < 25; ++iteration) {
        hybrid::NativeWorkerPreloader preloader(
            worker, L"unused-vl.vxd", worker, L"unused-sg.vxd", 44'100,
            true, true);
        preloader.start();
        preloader.stop();
    }
    return 0;
}

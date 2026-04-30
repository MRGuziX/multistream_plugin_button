#define CATCH_CONFIG_RUNNER
#include <catch2/catch_all.hpp>

#include <obs.h>

int main(int argc, char *argv[])
{
    if (!obs_startup("en-US", nullptr, nullptr)) {
        return 1;
    }
    const int result = Catch::Session().run(argc, argv);
    obs_shutdown();
    return result;
}

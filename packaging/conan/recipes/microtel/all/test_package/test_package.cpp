#include <microtel/provider.hpp>
#include <microtel/sdk_builder.hpp>
#include <microtel/status.hpp>
#include <microtel/version.hpp>

#include <chrono>
#include <iostream>

int main()
{
    // Nothing listens on port 1: Build() must still succeed, and Shutdown()
    // returns once its timeout expires. This links every layer of the SDK.
    auto built = microtel::SdkBuilder{}
                     .WithEndpoint("http://127.0.0.1:1")
                     .WithServiceName("test_package")
                     .Build();
    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }
    if ((*built)->Shutdown(std::chrono::milliseconds{100}) == microtel::Status::Failed)
    {
        std::cerr << "Shutdown() failed\n";
        return 1;
    }
    std::cout << "microtel " << microtel::kVersionString << '\n';
    return 0;
}

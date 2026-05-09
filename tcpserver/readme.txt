各平台操作步骤

Linux (Ubuntu/Debian)
# 1. 安装依赖
sudo apt install -y cmake g++ libasio-dev libspdlog-dev
# 2. 构建
cd tcpserver
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
# 3. 运行
./tcpserver

macOS
# 1. 安装依赖
brew install cmake asio spdlog


# 2. 构建
cd tcpserver
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(sysctl -n hw.ncpu)

# 3. 运行
./tcpserver

Windows (CMake + vcpkg)

# 1. 构建 (从仓库根目录)
cmake -S tcpserver -B tcpserver/build `
-DCMAKE_TOOLCHAIN_FILE=<vcpkg_root>/scripts/buildsystems/vcpkg.cmake `
-DCMAKE_BUILD_TYPE=Release
cmake --build tcpserver/build --config Release

# 2. 运行
.\tcpserver\build\Release\tcpserver.exe

Windows 上已有的 MSBuild/vcxproj 方式照常可用，两者互不影响。
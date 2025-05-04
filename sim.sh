#cmake -DUSE_SIMULATOR=ON -S . -DCMAKE_BUILD_TYPE=Debug -B build_sim
cmake -DUSE_SIMULATOR=ON -S . -B build_sim 
cmake --build build_sim
sudo sudo "PATH=$(pwd):$PATH" ./build_sim/pixelpilot

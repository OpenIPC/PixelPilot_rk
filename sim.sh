#cmake -DUSE_SIMULATOR=ON -S . -DCMAKE_BUILD_TYPE=Debug -B build_sim
for i in $(seq 1 25)
do
  touch /tmp/$i_video$i.mp4
done
cmake -DUSE_SIMULATOR=ON -S . -B build_sim 
cmake --build build_sim
sudo sudo "PATH=$(pwd):$PATH" ./build_sim/pixelpilot

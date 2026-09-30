rm -rf build
mkdir build
cd build
cmake -GNinja -DGUI_THEME=dark .. 
ninja
cd ..

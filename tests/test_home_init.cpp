cd /path/to/agentos/build
cmake --build . --target clean
cmake --build .
ctest --rerun-failed

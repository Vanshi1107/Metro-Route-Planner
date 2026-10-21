Put httplib.h (a single file from the cpp-httplib project) in this folder. It is not included; download it once:

  cd C:\dev
  git clone --depth 1 https://github.com/yhirose/cpp-httplib httplib-src
  Copy-Item C:\dev\httplib-src\httplib.h C:\dev\metro-planner\third_party\httplib\httplib.h
  git -C C:\dev\httplib-src rev-parse HEAD

Write down the long hash printed by the last command: it records exactly which version you used.
cpp-httplib is MIT licensed (c) Yuji Hirose. Then re-run cmake (see README.md) and metro_web will be built.

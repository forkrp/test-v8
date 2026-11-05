// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.

#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) return 2;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 3;
  }
  int (*smoke)(const char*) =
      (int (*)(const char*))dlsym(library, "V8JsonArm32Smoke");
  if (!smoke) return 4;
  int status = smoke(argc == 3 ? argv[2] : "");
  if (dlclose(library) != 0) return 5;
  return status;
}

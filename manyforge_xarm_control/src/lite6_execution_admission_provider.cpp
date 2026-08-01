// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0.
//
// Composable-cell relocation gate. The former executable-only body is exact
// historical I10/R acceptance input at ManyForge commit
// 08222188293df8e12f90dc72e204c21f22f92ea1, blob
// b15a09a83d2fc9cbbae48844123abf02f845e70e. It owned old lifecycle service
// authority and execution-admission publication; it is not retained as
// dormant code.

#include <iostream>

int main(int argc, char ** argv)
{
  (void) argc;
  (void) argv;
  std::cerr
    << "composable_cell_runtime_unavailable: Lite6 execution-admission publication requires the addressed I10/R lifecycle owner gate\n";
  return 78;
}

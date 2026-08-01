// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0.
//
// Composable-cell relocation gate. The former executable-only body is exact
// historical I10/R acceptance input at ManyForge commit
// 08222188293df8e12f90dc72e204c21f22f92ea1, blob
// 2dc16f13b494e65ddcbadd97f40dcfabec848aaa. It owned vendor observation,
// identity checks, and preflight evidence; it is not retained as dormant code.

#include <iostream>

int main(int argc, char ** argv)
{
  (void) argc;
  (void) argv;
  std::cerr
    << "composable_cell_runtime_unavailable: Lite6 supervised preflight requires the I10/R retained-product and vendor-observation gate\n";
  return 78;
}

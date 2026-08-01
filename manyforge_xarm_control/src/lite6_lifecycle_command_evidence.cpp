// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0.
//
// Composable-cell relocation gate. The former executable-only body is exact
// historical I10/R acceptance input at ManyForge commit
// 08222188293df8e12f90dc72e204c21f22f92ea1, blob
// 3f3e34db91be0a4022210389f0c82c774a68f12d. It owned attended lifecycle
// commands, result adjudication, and evidence serialization; it is not
// retained as dormant code.

#include <iostream>

int main(int argc, char ** argv)
{
  (void) argc;
  (void) argv;
  std::cerr
    << "composable_cell_runtime_unavailable: Lite6 lifecycle command evidence requires the I10/R lifecycle and attended-command gate\n";
  return 78;
}

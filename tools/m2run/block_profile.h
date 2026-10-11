// Block profile of the recompiled i960 code (m2run --profile): how often each
// basic block runs and how often control passes from one block to another.
// scripts/profile.py sums these over the scripts/inputs scenarios into
// profiles/<set>.txt, which m2recomp --profile lays the chunks out by.
//
// Format, one record a line ('#' lines are comments):
//   block ADDRESS INSTRUCTIONS COUNT   a block's first address (hex), its length, its executions
//   edge FROM TO COUNT                 control passed from block FROM to block TO (interrupts included)
#pragma once

#include <cstdint>
#include <string>

namespace gen::prof {

void start(); // counts from now on
void write(const std::string &path, uint64_t frames, uint64_t instructions);

} // namespace gen::prof

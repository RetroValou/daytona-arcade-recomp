// The SHARC core for a build without a recompiled SHARC program (every board
// but Model 2B, or a 2B set not recompiled yet): the core, and a
// run_generated that refuses to run. A Model 2B build compiles the
// generated program instead (m2sharcrecomp's output includes the core).
#include "runtime/sharc/sharc.cpp"

namespace rt::sharc {

#ifdef M2_SHARC_PROFILE
const uint32_t sharc_gen_count = 0;
const char *const sharc_gen_names[1] = {""};
const uint8_t sharc_gen_kinds[1] = {SharcPlain};
#endif

bool adsp21062_device::run_generated(uint64_t)
{
	throw SharcFatal("no recompiled SHARC program in this build (scripts/recompile.py makes it for a Model 2B set)");
}

} // namespace rt::sharc

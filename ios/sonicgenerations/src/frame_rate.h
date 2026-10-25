#pragma once

#include <rex/ppc/context.h>

// Reads sg_frame_rate once, before the guest starts. The generated
// instruction hook below only reads the resulting value.
void ConfigureFrameRate();

// C++ linkage and PPCRegister& match ReXGlue's generated mid-assembly hook ABI.
void SgPresentationInterval(PPCRegister& r11);

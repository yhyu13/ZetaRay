#pragma once

#include "../App/ZetaRay.h"

namespace ZetaRay::Model::glTF
{
	void Load(const char* modelRelPath, bool blenderToYupConversion = false) noexcept;
}
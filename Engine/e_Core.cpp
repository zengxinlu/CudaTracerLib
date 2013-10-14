#include <StdAfx.h>
#include "e_Core.h"


#include "..\MathTypes.h"
#include "e_RoughTransmittance.h"

void InitializeCuda4Tracer()
{
	cudaFree(0);
	SpectrumHelper::StaticInitialize();
	e_RoughTransmittanceManager::StaticInitialize();
}
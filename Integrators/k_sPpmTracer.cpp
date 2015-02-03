#include <StdAfx.h>
#include "k_sPpmTracer.h"
#include "..\Base\StringUtils.h"

#define LNG 200
#define SER_NAME "photonMapBuf.dat"

k_sPpmTracer::k_sPpmTracer()
: k_ProgressiveTracer(), m_pEntries(0), m_bFinalGather(false)
{
#ifdef NDEBUG
	m_uBlocksPerLaunch = 180;
#else
	m_uBlocksPerLaunch = 1;
#endif
	m_uPhotonsEmitted = -1;
	m_bLongRunning = false;
	unsigned int numPhotons = (m_uBlocksPerLaunch + 2) * PPM_slots_per_block;
	unsigned int linkedListLength = numPhotons * 10;
	m_sMaps = k_PhotonMapCollection<true>(numPhotons, LNG*LNG*LNG, linkedListLength);
}

void k_sPpmTracer::PrintStatus(std::vector<std::string>& a_Buf)
{
	double pC = floor((double)m_uPhotonsEmitted / 1000000.0);
	a_Buf.push_back(format("Photons emitted : %d[Mil]", (int)pC));
	double pCs = getValuePerSecond(m_uPhotonsEmitted, 1000000.0);
	a_Buf.push_back(format("Photons/Sec : %f", (float)pCs));
	a_Buf.push_back(format("Light Visibility : %f", m_fLightVisibility));
	a_Buf.push_back(format("Photons per pass : %d*100,000", m_sMaps.m_uPhotonBufferLength / 100000));
}

void k_sPpmTracer::CreateSliders(SliderCreateCallback a_Callback)
{
	//a_Callback(0.1f, 10.0f, true, &m_fInitialRadiusScale, "Initial radius = %g units");
	//a_Callback(0.1f, 100.0f, true, &m_uNewPhotonsPerRun, "Number of photons per pass = %g [M]");
}

void k_sPpmTracer::Resize(unsigned int _w, unsigned int _h)
{
	k_TracerBase::Resize(_w, _h);
	if(m_pEntries)
		CUDA_FREE(m_pEntries);
	CUDA_MALLOC(&m_pEntries, sizeof(k_AdaptiveEntry) * _w * _h);
}
/*
void print(k_PhotonMapCollection& m_sMaps, k_PhotonMap<k_HashGrid_Reg>& m_Map, std::string name)
{
	k_pPpmPhoton* photons = new k_pPpmPhoton[m_sMaps.m_uPhotonBufferLength];
	unsigned int* grid = new unsigned int[m_Map.m_uGridLength];
	cudaMemcpy(photons, m_sMaps.m_pPhotons, sizeof(k_pPpmPhoton) * m_sMaps.m_uPhotonBufferLength, cudaMemcpyDeviceToHost);
	cudaMemcpy(grid, m_Map.m_pDeviceHashGrid, sizeof(unsigned int) * m_Map.m_uGridLength, cudaMemcpyDeviceToHost);
			
	unsigned int usedCells = 0, maxCount = 0;
	unsigned char* counts = new unsigned char[m_Map.m_uGridLength];
	for(unsigned int i = 0; i < m_Map.m_uGridLength; i++)
	{
		if(grid[i] != -1)
		{
			usedCells++;
			k_pPpmPhoton* p = photons + grid[i];
			unsigned int c = 1;
			for( ; p->next != -1; c++)
				p = photons + p->next;
			counts[i] = c;
			maxCount = MAX(maxCount, c);
		}
		else
		{
			counts[i] = 0;
		}
	}
	OutputStream os(name.c_str());
	float avgNum = float(m_sMaps.m_uPhotonBufferLength) / float(usedCells), f1 = float(usedCells) / float(m_Map.m_uGridLength);
	os << m_Map.m_sHash.m_fGridSize;
	os << (int)avgNum * 2;
	os.Write(counts, sizeof(unsigned char) * m_Map.m_uGridLength);
	os.Close();
	float var = 0;
	int avg = (int)avgNum;
	for(unsigned int i = 0; i < m_Map.m_uGridLength; i++)
		if(counts[i])
			var += (counts[i] - avg) * (counts[i] - avg) / float(usedCells);
	std::string s = format("max : %d, avg : %f, used cells : %f, var : %f\n", maxCount, avgNum, f1, sqrtf(var));
	std::cout << s;
	OutputDebugString(s.c_str());
}*/

void k_sPpmTracer::DoRender(e_Image* I)
{
	k_ProgressiveTracer::DoRender(I);
	m_uPassesDone++;
	doPhotonPass();
	m_uPhotonsEmitted += m_sMaps.m_uPhotonNumEmitted;
	doEyePass(I);
	m_sMaps.StartNewPass();
}

void k_sPpmTracer::initNewPass(e_Image* I)
{
	k_ProgressiveTracer::StartNewTrace(I);
	m_uPhotonsEmitted = 0;
	AABB m_sEyeBox = GetEyeHitPointBox(m_pScene, m_pCamera, true);
	m_sEyeBox.Enlarge(0.1f);
	float r = fsumf(m_sEyeBox.maxV - m_sEyeBox.minV) / float(w);
	m_sEyeBox.minV -= make_float3(r);
	m_sEyeBox.maxV += make_float3(r);
	m_fInitialRadius = r;
	AABB volBox = m_pScene->getKernelSceneData().m_sVolume.box;
	for(unsigned int i = 0; i < m_pScene->getNodeCount(); i++)
	{
		e_StreamReference(e_Node) n = m_pScene->getNodes()(i);
		e_BufferReference<e_Mesh, e_KernelMesh> m = m_pScene->getMesh(n);
		unsigned int s = n->m_uMaterialOffset, l = m_pScene->getMesh(n)->m_sMatInfo.getLength();
		for(unsigned int j = 0; j < l; j++)
		{
			e_StreamReference(e_KernelMaterial) mat = m_pScene->m_pMaterialBuffer->operator()(s + j, 1);
			const e_KernelBSSRDF* bssrdf;
			DifferentialGeometry dg;
			ZERO_MEM(dg);
			if(mat->GetBSSRDF(dg, &bssrdf))
			{
				volBox.Enlarge(m_pScene->getBox(n));
				m_bLongRunning |= 1;
			}
		}
	}
	//volBox = m_pScene->getKernelSceneData().m_sBox;
	m_sMaps.StartNewRendering(m_sEyeBox, volBox, r);
	m_sMaps.StartNewPass();

	float r_scene = length(m_pScene->getKernelSceneData().m_sBox.Size()) / 2;
	r_min = 10e-7f * r_scene;
	r_max = 10e-3f * r_scene;
	float r1 = r_max/10.0f;
	doStartPass(r1, r1);
}

void k_sPpmTracer::StartNewTrace(e_Image* I)
{
	m_bDirect = !m_pScene->getVolumes().getLength();
#ifndef _DEBUG
	m_fLightVisibility = k_Tracer::GetLightVisibility(m_pScene, m_pCamera, 1);
#endif
	if (m_bDirect)
		m_bDirect = m_fLightVisibility > 0.5f;
	//m_bDirect = 0;
	initNewPass(I);
}
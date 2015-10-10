#include <StdAfx.h>
#include "../Engine/e_Buffer.h"
#include "k_sPpmTracer.h"
#include "..\Base\Timer.h"
#include "../Engine/e_DynamicScene.h"
#include "../Engine/e_Node.h"
#include "../Engine/e_Mesh.h"

k_sPpmTracer::k_sPpmTracer()
	: m_pEntries(0), m_bFinalGather(false), m_fLightVisibility(1)
{
#ifdef NDEBUG
	m_uBlocksPerLaunch = 180;
#else
	m_uBlocksPerLaunch = 1;
#endif
	m_uTotalPhotonsEmitted = -1;
	unsigned int numPhotons = (m_uBlocksPerLaunch + 2) * PPM_slots_per_block;
	m_sSurfaceMap = e_SpatialLinkedMap<k_pPpmPhoton>(250, numPhotons * PPM_MaxRecursion / 5);
	//m_pVolumeEstimator = new k_PointStorage(100, numPhotons * PPM_MaxRecursion / 2);
	m_pVolumeEstimator = new k_BeamGrid(100, numPhotons * PPM_MaxRecursion / 10);
	//m_pVolumeEstimator = new k_BeamBVHStorage(numPhotons/50, 0);
	//m_pVolumeEstimator = new k_BeamBeamGrid(30, numPhotons * PPM_MaxRecursion / 2);
}

void k_sPpmTracer::PrintStatus(std::vector<std::string>& a_Buf) const
{
	double pC = floor((double)m_uTotalPhotonsEmitted / 1000000.0);
	a_Buf.push_back(format("Photons emitted : %d[Mil]", (int)pC));
	double pCs = m_uTotalPhotonsEmitted / m_fAccRuntime / 1000000.0f;
	a_Buf.push_back(format("Photons/Sec : %f", (float)pCs));
	a_Buf.push_back(format("Light Visibility : %f", m_fLightVisibility));
	a_Buf.push_back(format("Photons per pass : %d*100,000", m_sSurfaceMap.numData / 100000));
	a_Buf.push_back(format("Final gather %d", m_bFinalGather));
	a_Buf.push_back(format("%.2f%% Surf Photons", (float)m_sSurfaceMap.deviceDataIdx / m_sSurfaceMap.numData * 100));
	a_Buf.push_back("Volumeric Estimator : ");
	m_pVolumeEstimator->PrintStatus(a_Buf);
}

void k_sPpmTracer::CreateSliders(SliderCreateCallback a_Callback) const
{
	//a_Callback(0.1f, 10.0f, true, &m_fInitialRadiusScale, "Initial radius = %g units");
	//a_Callback(0.1f, 100.0f, true, &m_uNewPhotonsPerRun, "Number of photons per pass = %g [M]");
}

void k_sPpmTracer::Resize(unsigned int _w, unsigned int _h)
{
	k_Tracer<true, true>::Resize(_w, _h);
	if(m_pEntries)
		CUDA_FREE(m_pEntries);
	//CUDA_MALLOC(&m_pEntries, sizeof(k_AdaptiveEntry) * _w * _h);
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
			maxCount = max(maxCount, c);
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
	std::string s = format("max : %d, avg : %f, used cells : %f, var : %f\n", maxCount, avgNum, f1, math::sqrt(var));
	std::cout << s;
	OutputDebugString(s.c_str());
}*/
static Vec2i lastPix = Vec2i(0,0);
void k_sPpmTracer::DoRender(e_Image* I)
{
	//I->Clear();
	doPhotonPass();
	m_uTotalPhotonsEmitted += m_uPhotonEmittedPass;
	k_Tracer<true, true>::DoRender(I);

	//k_AdaptiveStruct str(r_min, r_max, m_pEntries, w, m_uPassesDone);
	//k_AdaptiveEntry ent;
	//ThrowCudaErrors(cudaMemcpy(&ent, &str(lastPix.x, lastPix.y), sizeof(ent), cudaMemcpyDeviceToHost));
	//printf("{r = %f, rd = %f}, r = %f, {min = %f, max = %f}\n", ent.r, ent.rd, getCurrentRadius2(2), str.r_min, str.r_max);
}

void k_sPpmTracer::Debug(e_Image* I, const Vec2i& pixel)
{
	lastPix = pixel;
}

void k_sPpmTracer::StartNewTrace(e_Image* I)
{
	m_bDirect = !m_pScene->getVolumes().hasElements();
#ifndef _DEBUG
	m_fLightVisibility = k_Tracer::GetLightVisibility(m_pScene, 1);
#endif
	if (m_bDirect)
		m_bDirect = m_fLightVisibility > 0.5f;
	//m_bDirect = 0;
	k_Tracer<true, true>::StartNewTrace(I);
	m_uTotalPhotonsEmitted = 0;
#ifndef _DEBUG
	AABB m_sEyeBox = GetEyeHitPointBox(m_pScene, true);
#else
	AABB m_sEyeBox = m_pScene->getSceneBox();
#endif
	float r = (m_sEyeBox.maxV - m_sEyeBox.minV).sum() / float(w);
	m_sEyeBox.minV -= Vec3f(r);
	m_sEyeBox.maxV += Vec3f(r);
	m_fInitialRadius = r;
	AABB volBox = m_pScene->getKernelSceneData().m_sVolume.box;
	for (auto it : m_pScene->getNodes())
	{
		e_StreamReference<e_KernelMaterial> mats = m_pScene->getMats(it);
		for (unsigned int j = 0; j < mats.getLength(); j++)
		{
			const e_KernelBSSRDF* bssrdf;
			DifferentialGeometry dg;
			ZERO_MEM(dg);
			if (mats(j)->GetBSSRDF(dg, &bssrdf))
			{
				volBox = volBox.Extend(m_pScene->getNodeBox(it));
			}
		}
	}
	m_sSurfaceMap.SetSceneDimensions(m_sEyeBox, r);
	m_pVolumeEstimator->StartNewRendering(volBox, r);

	float r_scene = m_sEyeBox.Size().length() / 2;
	r_min = 10e-7f * r_scene;
	r_max = 10e-3f * r_scene;
	float r1 = r_max;

	doStartPass(r1, r1);
}
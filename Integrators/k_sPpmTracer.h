#pragma once

#include "..\Kernel\k_Tracer.h"
#include "..\Base\CudaRandom.h"
#include "..\Base\Timer.h"
#include <time.h>
#include "..\Engine\e_Grid.h"
#include "../Math//Compression.h"

#define ALPHA (2.0f / 3.0f)

CUDA_FUNC_IN Ray BSSRDF_Entry(const e_KernelBSSRDF* bssrdf, const Frame& sys, const float3& pos, const float3& wi)
{
	float3 dir = VectorMath::refract(wi, sys.n, 1.0f / bssrdf->e);
	return Ray(pos, dir);
}

CUDA_FUNC_IN Ray BSSRDF_Exit(const e_KernelBSSRDF* bssrdf, const Frame& sys, const float3& pos, const float3& wi)
{
	float3 dir = VectorMath::refract(wi, sys.n, 1.0f / bssrdf->e);
	return Ray(pos, dir);
}

struct k_AdaptiveEntry
{
	float r, rd;
	float psi, psi2;
	float I, I2;
	float pl;
};

struct k_AdaptiveStruct
{
	k_AdaptiveEntry* E;
	float r_min;
	float r_max;
	CUDA_FUNC_IN k_AdaptiveStruct(){}
	k_AdaptiveStruct(float rmin, float rmax, k_AdaptiveEntry* e)
	{
		E = e;
		r_min = rmin;
		r_max = rmax;
	}
};

struct k_pPpmPhoton
{
private:
	float3 Pos;
	//Spectrum L;
	//float3 Wi;
	//float3 Nor;
	RGBE L;
	unsigned short Wi;
	unsigned short Nor;
public:
	unsigned int next;
	unsigned int typeFlag;
	CUDA_FUNC_IN k_pPpmPhoton(){}
	CUDA_FUNC_IN k_pPpmPhoton(const float3& p, const Spectrum& l, const float3& wi, const float3& n, unsigned int ne, unsigned int type)
	{
		Pos = p;
		Nor = NormalizedFloat3ToUchar2(n);
		L = (l).toRGBE();
		Wi = NormalizedFloat3ToUchar2(wi);
		next = ne;
		typeFlag = type;
	}
	CUDA_FUNC_IN float3 getNormal()
	{
		return Uchar2ToNormalizedFloat3(Nor);
	}
	CUDA_FUNC_IN float3 getWi()
	{
		return Uchar2ToNormalizedFloat3(Wi);
	}
	CUDA_FUNC_IN Spectrum getL()
	{
		Spectrum s;
		s.fromRGBE(L);
		return s;
	}
	CUDA_FUNC_IN float3 getPos()
	{/*
		uint4 r;
		r.x = Pos;
		r.y = (L.x << 24) | (L.y << 16) | (L.z << 8) | L.w;
		r.z = (L.x << 24) | (L.y << 16) | (L.z << 8) | L.w;
		r.w = next;
		return r;*/
		return Pos;
	}/*
	CUDA_FUNC_IN k_pPpmPhoton(const uint4& v)
	{
		Pos = v.x;
		L = *(RGBE*)&v.y;
		Wi = make_uchar2(v.z & 0xff, (v.z >> 8) & 0xff);
		Nor = make_uchar2((v.z >> 16) & 0xff, v.z >> 24);
		next = v.w;
	}*/
};

enum k_StoreResult
{
	Success = 1,
	NotValid = 2,
	Full = 3,
};

template<typename HASH> struct k_PhotonMap
{
	k_pPpmPhoton* m_pDevicePhotons;
	unsigned int* m_pDeviceHashGrid;
	HASH m_sHash;
	unsigned int m_uMaxPhotonCount;
	unsigned int m_uGridLength;

	CUDA_FUNC_IN k_PhotonMap()
	{

	}

	CUDA_FUNC_IN k_PhotonMap(unsigned int photonN, unsigned int hashN, k_pPpmPhoton* P)
	{
		m_uGridLength = hashN;
		m_pDevicePhotons = P;
		m_pDeviceHashGrid = 0;
		CUDA_MALLOC(&m_pDeviceHashGrid, sizeof(unsigned int) * m_uGridLength);
		cudaMemset(m_pDeviceHashGrid, -1, sizeof(unsigned int) * m_uGridLength);
		m_uMaxPhotonCount = photonN;
	}

	void Serialize(OutputStream& O, void* hostbuf)
	{
		O << m_uMaxPhotonCount;
		O << m_uGridLength;
		cudaMemcpy(hostbuf, m_pDeviceHashGrid, sizeof(unsigned int) * m_uGridLength, cudaMemcpyDeviceToHost);
		O.Write(hostbuf, sizeof(unsigned int) * m_uGridLength);
		O.Write(m_sHash);
	}

	void DeSerialize(IInStream& I, void* hostbuf)
	{
		I >> m_uMaxPhotonCount;
		I >> m_uGridLength;
		I.Read(hostbuf, sizeof(unsigned int) * m_uGridLength);
		cudaMemcpy(m_pDeviceHashGrid, hostbuf, sizeof(unsigned int) * m_uGridLength, cudaMemcpyHostToDevice);
		I.Read(m_sHash);
	}

	void Free()
	{
		CUDA_FREE(m_pDeviceHashGrid);
	}

	void StartNewPass()
	{
		cudaMemset(m_pDeviceHashGrid, -1, sizeof(unsigned int) * m_uGridLength);
	}

	void Resize(unsigned int N, k_pPpmPhoton* P)
	{
		m_pDevicePhotons = P;
		m_uMaxPhotonCount = N;
	}

	void StartNewRendering(const AABB& box, float a_InitRadius)
	{
		m_sHash = HASH(box, a_InitRadius, m_uGridLength);
		cudaMemset(m_pDeviceHashGrid, -1, sizeof(unsigned int) * m_uGridLength);
	}

#ifdef __CUDACC__
	CUDA_FUNC_IN k_StoreResult StorePhoton(const float3& p, const Spectrum& l, const float3& wi, const float3& n, unsigned int* a_PhotonCounter) const;
#endif
};

struct k_PhotonMapCollection
{
	k_PhotonMap<k_HashGrid_Reg> m_sVolumeMap;
	k_PhotonMap<k_HashGrid_Reg> m_sSurfaceMap;
	k_pPpmPhoton* m_pPhotons;
	unsigned int m_uPhotonBufferLength;
	unsigned int m_uPhotonNumStored;
	unsigned int m_uPhotonNumEmitted;
	unsigned int m_uRealBufferSize;

	CUDA_FUNC_IN k_PhotonMapCollection()
	{

	}

	void Serialize(OutputStream& O)
	{
		void* hostbuf = malloc(m_uPhotonBufferLength * sizeof(k_pPpmPhoton));
		O << m_uPhotonBufferLength;
		O << m_uPhotonNumStored;
		O << m_uPhotonNumEmitted;
		O << m_uRealBufferSize;
		cudaMemcpy(hostbuf, m_pPhotons, m_uPhotonBufferLength * sizeof(k_pPpmPhoton), cudaMemcpyDeviceToHost);
		O.Write(hostbuf, m_uPhotonBufferLength * sizeof(k_pPpmPhoton));
		m_sVolumeMap.Serialize(O, hostbuf);
		m_sSurfaceMap.Serialize(O, hostbuf);
		free(hostbuf);
	}

	void DeSerialize(InputStream& I)
	{
		void* hostbuf = malloc(m_uPhotonBufferLength * sizeof(k_pPpmPhoton));
		I >> m_uPhotonBufferLength;
		I >> m_uPhotonNumStored;
		I >> m_uPhotonNumEmitted;
		I >> m_uRealBufferSize;
		I.Read(hostbuf, m_uPhotonBufferLength * sizeof(k_pPpmPhoton));
		cudaMemcpy(m_pPhotons, hostbuf, m_uPhotonBufferLength * sizeof(k_pPpmPhoton), cudaMemcpyHostToDevice);
		m_sVolumeMap.DeSerialize(I, hostbuf);
		m_sSurfaceMap.DeSerialize(I, hostbuf);
		free(hostbuf);
	}

	k_PhotonMapCollection(unsigned int a_BufferLength, unsigned int a_HashNum);

	void Free();

	void Resize(unsigned int a_BufferLength);

	void StartNewPass();

	bool PassFinished();

	void StartNewRendering(const AABB& sbox, const AABB& vbox, float a_R)
	{
		m_sVolumeMap.StartNewRendering(vbox, a_R);
		m_sSurfaceMap.StartNewRendering(sbox, a_R);
	}

#ifdef __CUDACC__

	template<bool SURFACE> CUDA_FUNC_IN k_StoreResult StorePhoton(const float3& p, const Spectrum& l, const float3& wi, const float3& n)
	{
		if(SURFACE)
			return m_sSurfaceMap.StorePhoton(p, l, wi, n, &m_uPhotonNumStored);
		else return m_sVolumeMap.StorePhoton(p, l, wi, n, &m_uPhotonNumStored);
	}
#endif
};

enum
{
	PPM_Photons_Per_Thread = 12,
	PPM_BlockX = 32,
	PPM_BlockY = 6,
	PPM_MaxRecursion = 6,

	PPM_photons_per_block = PPM_Photons_Per_Thread * PPM_BlockX * PPM_BlockY,
	PPM_slots_per_thread = PPM_Photons_Per_Thread * PPM_MaxRecursion,
	PPM_slots_per_block = PPM_photons_per_block * PPM_MaxRecursion,
};

class k_sPpmTracer : public k_ProgressiveTracer
{
private:
	k_PhotonMapCollection m_sMaps;
	bool m_bDirect;
	float m_fLightVisibility;

	float m_fInitialRadius;
	unsigned long long m_uPhotonsEmitted;
	unsigned long long m_uPreviosCount;

	float m_fInitialRadiusScale;
	const unsigned int m_uGridLength;

	unsigned int m_uBlocksPerLaunch;

	float m_uNewPhotonsPerRun;
	int m_uModus;
	bool m_bLongRunning;

	k_AdaptiveEntry* m_pEntries;
	float r_min, r_max;
public:
	k_sPpmTracer();
	virtual ~k_sPpmTracer()
	{
		m_sMaps.Free();
	}
	virtual void Resize(unsigned int _w, unsigned int _h);
	virtual void Debug(e_Image* I, int2 pixel);
	virtual void PrintStatus(std::vector<std::string>& a_Buf);
	virtual void CreateSliders(SliderCreateCallback a_Callback);
protected:
	virtual void DoRender(e_Image* I);
	virtual void StartNewTrace(e_Image* I);
private:
	void initNewPass(e_Image* I);
	void doPhotonPass();
	void doEyePass(e_Image* I);
	void doStartPass(float r, float rd);
	void updateBuffer()
	{
		unsigned int N = unsigned int(m_uNewPhotonsPerRun * 1000000.0f);
		if(N != m_sMaps.m_uPhotonBufferLength)
			m_sMaps.Resize(N);
	}
	float getCurrentRadius(int exp)
	{
		float f = 1;
		for(unsigned int k = 1; k < m_uPassesDone; k++)
			f *= (k + ALPHA) / k;
		f = powf(m_fInitialRadius, float(exp)) * f * 1.0f / float(m_uPassesDone);
		return powf(f, 1.0f / float(exp));
	}
};
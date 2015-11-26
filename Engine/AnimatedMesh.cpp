#include "StdAfx.h"
#include "AnimatedMesh.h"
#include "Buffer.h"
#include <MathTypes.h>
#include <Base/Platform.h>
#include <Base/FileStream.h>
#include "TriangleData.h"
#include "Material.h"
#include "TriIntersectorData.h"

namespace CudaTracerLib {

BufferReference<char, char> malloc_aligned(Stream<char>* stream, unsigned int a_Count, unsigned int a_Alignment)
{
	BufferReference<char, char> ref = stream->malloc(a_Count + a_Alignment * 2);
	uintptr_t ptr = (uintptr_t)ref.getDevice();
	unsigned int diff = ptr % a_Alignment, off = a_Alignment - diff;
	if (diff)
	{
		unsigned int end = ref.getIndex() + off + a_Count;
		BufferReference<char, char> refFreeFront = BufferReference<char, char>(stream, ref.getIndex(), off),
			refFreeTail = BufferReference<char, char>(stream, end, ref.getLength() - off - a_Count);
		stream->dealloc(refFreeFront);
		if (refFreeTail.getLength() != 0)
			stream->dealloc(refFreeTail);
		return BufferReference<char, char>(stream, ref.getIndex() + off, a_Count);
	}
	else
	{
		BufferReference<char, char> refFreeTail = BufferReference<char, char>(stream, ref.getIndex() + a_Count, a_Alignment * 2);
		stream->dealloc(refFreeTail);
		return BufferReference<char, char>(stream, ref.getIndex() + off, a_Count);
	}
}

void AnimationFrame::serialize(FileOutputStream& a_Out)
{
	a_Out << (size_t)m_sHostConstructionData.size();
	a_Out.Write(&m_sHostConstructionData[0], sizeof(float4x4) * (unsigned int)m_sHostConstructionData.size());
}

void AnimationFrame::deSerialize(IInStream& a_In, Stream<char>* Buf)
{
	size_t A;
	a_In >> A;
	//m_sMatrices = Buf->malloc(sizeof(float4x4) * (unsigned int)A);
	m_sMatrices = malloc_aligned(Buf, sizeof(float4x4) * (unsigned int)A, 16);
	a_In >> m_sMatrices;
}

void Animation::serialize(FileOutputStream& a_Out)
{
	a_Out << (size_t)m_pFrames.size();
	a_Out << m_uFrameRate;
	a_Out.Write(m_sName);
	for (unsigned int i = 0; i < m_pFrames.size(); i++)
		m_pFrames[i].serialize(a_Out);
}

void Animation::deSerialize(IInStream& a_In, Stream<char>* Buf)
{
	size_t m_uNumFrames;
	a_In >> m_uNumFrames;
	a_In >> m_uFrameRate;
	a_In.Read(m_sName);
	for (unsigned int i = 0; i < m_uNumFrames; i++)
	{
		m_pFrames.push_back(AnimationFrame());
		m_pFrames[i].deSerialize(a_In, Buf);
	}
}

AnimatedMesh::AnimatedMesh(const std::string& path, IInStream& a_In, Stream<TriIntersectorData>* a_Stream0, Stream<TriangleData>* a_Stream1, Stream<BVHNodeData>* a_Stream2, Stream<TriIntersectorData2>* a_Stream3, Stream<Material>* a_Stream4, Stream<char>* a_Stream5)
	: Mesh(path, a_In, a_Stream0, a_Stream1, a_Stream2, a_Stream3, a_Stream4, a_Stream5)
{
	m_uType = MESH_ANIMAT_TOKEN;
	a_In.Read(&k_Data, sizeof(k_Data));
	for (unsigned int i = 0; i < k_Data.m_uAnimCount; i++)
	{
		Animation A;
		A.deSerialize(a_In, a_Stream5);
		m_pAnimations.push_back(A);
	}
	m_sVertices = a_Stream5->malloc(sizeof(AnimatedVertex) * k_Data.m_uVertexCount);//malloc_aligned(a_Stream5, sizeof(AnimatedVertex) * k_Data.m_uVertexCount, 16);//
	a_In >> m_sVertices;
	m_sTriangles = a_Stream5->malloc(sizeof(uint3) * m_sTriInfo.getLength());//malloc_aligned(a_Stream5, sizeof(uint3) * m_sTriInfo.getLength(), 16);//
	a_In >> m_sTriangles;
	uint3* idx = (uint3*)m_sTriangles.operator char *() + (m_sTriInfo.getLength() - 2);
	a_Stream5->UpdateInvalidated();
	m_pBuilder = 0;
}

void AnimatedMesh::CreateNewMesh(AnimatedMesh* A, Stream<TriIntersectorData>* a_Stream0, Stream<TriangleData>* a_Stream1, Stream<BVHNodeData>* a_Stream2, Stream<TriIntersectorData2>* a_Stream3, Stream<Material>* a_Stream4, Stream<char>* a_Stream5)
{
	A->m_uType = MESH_ANIMAT_TOKEN;
	A->m_sLocalBox = m_sLocalBox;
	A->m_sMatInfo = m_sMatInfo;
	A->m_sIndicesInfo = a_Stream3->malloc(m_sIndicesInfo, true);
	A->m_sTriInfo = a_Stream1->malloc(m_sTriInfo, true);
	A->m_sNodeInfo = a_Stream2->malloc(m_sNodeInfo, true);
	A->m_sIntInfo = a_Stream0->malloc(m_sIntInfo, true);
	A->m_pBuilder = 0;

	A->k_Data = k_Data;
	A->m_pAnimations = m_pAnimations;
	A->m_sVertices = m_sVertices;
	A->m_sTriangles = m_sTriangles;
}

}
#include <StdAfx.h>
#include "Node.h"
#include <Engine/Mesh.h>
#include <Base/Buffer.h>
#include <Engine/Material.h>
#include <Engine/TriIntersectorData.h>

namespace CudaTracerLib {

Node::Node(unsigned int MeshIndex, Mesh* mesh, StreamReference<Material> mat)
	: m_uInstanciatedMaterial(false)
{
	m_uMeshIndex = MeshIndex;
	m_uMaterialOffset = mat.getIndex();
	for (unsigned int i = 0; i < mesh->m_sMatInfo.getLength(); i++)
		mat(i) = *mesh->m_sMatInfo(i);
}

AABB Node::getWorldBox(Mesh* mesh, const float4x4& mat) const
{
	return mesh->m_sLocalBox.Transform(mat);
}

}
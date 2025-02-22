/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzCore/Jobs/JobFunction.h>
#include <AzCore/Jobs/JobCompletion.h>
#include "EMotionFXConfig.h"
#include "DualQuatSkinDeformer.h"
#include "Mesh.h"
#include "Node.h"
#include "SubMesh.h"
#include "Actor.h"
#include "SkinningInfoVertexAttributeLayer.h"
#include "TransformData.h"
#include "ActorInstance.h"
#include <EMotionFX/Source/Allocators.h>
#include <MCore/Source/LogManager.h>

namespace EMotionFX
{
    AZ_CLASS_ALLOCATOR_IMPL(DualQuatSkinDeformer, DeformerAllocator, 0)

    DualQuatSkinDeformer::DualQuatSkinDeformer(Mesh* mesh)
        : MeshDeformer(mesh)
    {
    }

    DualQuatSkinDeformer::~DualQuatSkinDeformer()
    {
    }

    AZ::Outcome<size_t> DualQuatSkinDeformer::FindLocalBoneIndex(uint32 nodeIndex) const
    {
        const size_t numBones = m_bones.size();
        for (size_t i = 0; i < numBones; ++i)
        {
            if (m_bones[i].mNodeNr == nodeIndex)
            {
                return AZ::Success(i);
            }
        }

        return AZ::Failure();
    }

    DualQuatSkinDeformer* DualQuatSkinDeformer::Create(Mesh* mesh)
    {
        return aznew DualQuatSkinDeformer(mesh);
    }

    uint32 DualQuatSkinDeformer::GetType() const
    {
        return TYPE_ID;
    }

    uint32 DualQuatSkinDeformer::GetSubType() const
    {
        return SUBTYPE_ID;
    }

    MeshDeformer* DualQuatSkinDeformer::Clone(Mesh* mesh)
    {
        // create the new cloned deformer
        DualQuatSkinDeformer* result = aznew DualQuatSkinDeformer(mesh);

        // copy the bone info (for precalc/optimization reasons)
        result->m_bones = m_bones;

        // return the result
        return result;
    }

    // the main method where all calculations are done
    void DualQuatSkinDeformer::Update(ActorInstance* actorInstance, [[maybe_unused]] Node* node, [[maybe_unused]] float timeDelta)
    {
        const Actor* actor = actorInstance->GetActor();
        const Pose* pose = actorInstance->GetTransformData()->GetCurrentPose();
        const uint32 numVertices = mMesh->GetNumVertices();

        // pre-calculate the skinning matrices
        for (BoneInfo& boneInfo : m_bones)
        {
            const uint32 nodeIndex = boneInfo.mNodeNr;
            const Transform skinTransform = actor->GetInverseBindPoseTransform(nodeIndex) * pose->GetModelSpaceTransform(nodeIndex);
            boneInfo.mDualQuat.FromRotationTranslation(skinTransform.mRotation, skinTransform.mPosition);
        }

        AZ::JobCompletion jobCompletion;

        // Split up the skinned vertices into batches.
        const AZ::u32 numBatches = aznumeric_caster(ceilf(aznumeric_cast<float>(numVertices) / aznumeric_cast<float>(s_numVerticesPerBatch)));
        for (AZ::u32 batchIndex = 0; batchIndex < numBatches; ++batchIndex)
        {
            const AZ::u32 startVertex = batchIndex * s_numVerticesPerBatch;
            const AZ::u32 endVertex = AZStd::min(startVertex + s_numVerticesPerBatch, numVertices);

            // Create a job for every batch and skin them simultaneously.
            AZ::JobContext* jobContext = nullptr;
            AZ::Job* job = AZ::CreateJobFunction([this, startVertex, endVertex]()
                {
                    SkinRange(mMesh, startVertex, endVertex, m_bones);
                }, /*isAutoDelete=*/true, jobContext);

            job->SetDependent(&jobCompletion);
            job->Start();
        }

        jobCompletion.StartAndWaitForCompletion();
    }

    void DualQuatSkinDeformer::SkinRange(Mesh* mesh, AZ::u32 startVertex, AZ::u32 endVertex, const AZStd::vector<BoneInfo>& boneInfos)
    {
        SkinningInfoVertexAttributeLayer* layer = (SkinningInfoVertexAttributeLayer*)mesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID);
        AZ_Assert(layer, "Cannot find skinning layer.");

        AZ::Vector3 newTangent;
        AZ::Vector3 vtxPos, normal, tangent, bitangent;
        AZ::u32 orgVertex;
        float weight;

        AZ::Vector3* positions = static_cast<AZ::Vector3*>(mesh->FindVertexData(Mesh::ATTRIB_POSITIONS));
        AZ::Vector3* normals = static_cast<AZ::Vector3*>(mesh->FindVertexData(Mesh::ATTRIB_NORMALS));
        AZ::Vector4* tangents = static_cast<AZ::Vector4*>(mesh->FindVertexData(Mesh::ATTRIB_TANGENTS));
        AZ::Vector3* bitangents = static_cast<AZ::Vector3*>(mesh->FindVertexData(Mesh::ATTRIB_BITANGENTS));
        AZ::u32* orgVerts = static_cast<AZ::u32*>(mesh->FindVertexData(Mesh::ATTRIB_ORGVTXNUMBERS));

        // if there are tangents and bitangents to skin
        if (tangents && bitangents)
        {
            for (AZ::u32 v = startVertex; v < endVertex; ++v)
            {
                orgVertex = orgVerts[v];
                vtxPos = positions[v];
                normal = normals[v];
                const float tangentW = tangents[v].GetW();
                tangent.Set(tangents[v].GetX(), tangents[v].GetY(), tangents[v].GetZ());
                bitangent = bitangents[v];

                // process the skin influences for this vertex
                const size_t numInfluences = layer->GetNumInfluences(orgVertex);
                if (numInfluences > 0)
                {
                    // get the pivot quat, used for the dot product check
                    const MCore::DualQuaternion& pivotQuat = boneInfos[ layer->GetInfluence(orgVertex, 0)->GetBoneNr() ].mDualQuat;

                    // our skinning dual quaternion
                    MCore::DualQuaternion skinQuat(AZ::Quaternion(0, 0, 0, 0), AZ::Quaternion(0, 0, 0, 0));

                    for (size_t i = 0; i < numInfluences; ++i)
                    {
                        SkinInfluence* influence = layer->GetInfluence(orgVertex, i);
                        weight = influence->GetWeight();

                        // check if we need to invert the dual quat
                        MCore::DualQuaternion influenceQuat = boneInfos[ influence->GetBoneNr() ].mDualQuat;
                        if (influenceQuat.mReal.Dot(pivotQuat.mReal) < 0.0f)
                        {
                            influenceQuat *= -1.0f;
                        }

                        // weighted sum
                        skinQuat += influenceQuat * weight;
                    }

                    // normalize the dual quaternion
                    skinQuat.Normalize();

                    // perform skinning
                    positions[v] = skinQuat.TransformPoint(vtxPos);
                    normals[v] = skinQuat.TransformVector(normal);
                    newTangent = skinQuat.TransformVector(tangent);
                    tangents[v].Set(newTangent.GetX(), newTangent.GetY(), newTangent.GetZ(), tangentW);
                    bitangents[v] = skinQuat.TransformVector(bitangent);
                }
                else
                {
                    // no skinning influences, just copy the values
                    positions[v] = vtxPos;
                    normals[v] = normal;
                    newTangent = tangent;
                    tangents[v].Set(newTangent.GetX(), newTangent.GetY(), newTangent.GetZ(), tangentW);
                    bitangents[v] = bitangent;
                }
            }
        }
        else if (tangents && !bitangents) // tangents but no bitangents
        {
            for (AZ::u32 v = startVertex; v < endVertex; ++v)
            {
                orgVertex = orgVerts[v];
                vtxPos = positions[v];
                normal = normals[v];
                const float tangentW = tangents[v].GetW();
                tangent.Set(tangents[v].GetX(), tangents[v].GetY(), tangents[v].GetZ());

                // process the skin influences for this vertex
                const size_t numInfluences = layer->GetNumInfluences(orgVertex);
                if (numInfluences > 0)
                {
                    // get the pivot quat, used for the dot product check
                    const MCore::DualQuaternion& pivotQuat = boneInfos[ layer->GetInfluence(orgVertex, 0)->GetBoneNr() ].mDualQuat;

                    // our skinning dual quaternion
                    MCore::DualQuaternion skinQuat(AZ::Quaternion(0, 0, 0, 0), AZ::Quaternion(0, 0, 0, 0));

                    for (size_t i = 0; i < numInfluences; ++i)
                    {
                        SkinInfluence* influence = layer->GetInfluence(orgVertex, i);
                        weight = influence->GetWeight();

                        // check if we need to invert the dual quat
                        MCore::DualQuaternion influenceQuat = boneInfos[ influence->GetBoneNr() ].mDualQuat;
                        if (influenceQuat.mReal.Dot(pivotQuat.mReal) < 0.0f)
                        {
                            influenceQuat *= -1.0f;
                        }

                        // weighted sum
                        skinQuat += influenceQuat * weight;
                    }

                    // normalize the dual quaternion
                    skinQuat.Normalize();

                    // perform skinning
                    positions[v] = skinQuat.TransformPoint(vtxPos);
                    normals[v] = skinQuat.TransformVector(normal);
                    newTangent = skinQuat.TransformVector(tangent);
                    tangents[v].Set(newTangent.GetX(), newTangent.GetY(), newTangent.GetZ(), tangentW);
                }
                else
                {
                    // no skinning influences, just copy the values
                    positions[v] = vtxPos;
                    normals[v] = normal;
                    newTangent = tangent;
                    tangents[v].Set(newTangent.GetX(), newTangent.GetY(), newTangent.GetZ(), tangentW);
                }
            }
        }
        else // there are no tangents and bitangents to skin
        {
            for (AZ::u32 v = startVertex; v < endVertex; ++v)
            {
                orgVertex = orgVerts[v];
                vtxPos = positions[v];
                normal = normals[v];

                // process the skin influences for this vertex
                const size_t numInfluences = layer->GetNumInfluences(orgVertex);
                if (numInfluences > 0)
                {
                    // get the pivot quat, used for the dot product check
                    const MCore::DualQuaternion& pivotQuat = boneInfos[ layer->GetInfluence(orgVertex, 0)->GetBoneNr() ].mDualQuat;

                    // our skinning dual quaternion
                    MCore::DualQuaternion skinQuat(AZ::Quaternion(0, 0, 0, 0), AZ::Quaternion(0, 0, 0, 0));

                    for (size_t i = 0; i < numInfluences; ++i)
                    {
                        SkinInfluence* influence = layer->GetInfluence(orgVertex, i);
                        weight = influence->GetWeight();

                        // check if we need to invert the dual quat
                        MCore::DualQuaternion influenceQuat = boneInfos[ influence->GetBoneNr() ].mDualQuat;
                        if (influenceQuat.mReal.Dot(pivotQuat.mReal) < 0.0f)
                        {
                            influenceQuat *= -1.0f;
                        }

                        // weighted sum
                        skinQuat += influenceQuat * weight;
                    }

                    // normalize the dual quaternion
                    skinQuat.Normalize();

                    // perform skinning
                    positions[v] = skinQuat.TransformPoint(vtxPos);
                    normals[v] = skinQuat.TransformVector(normal);
                }
                else
                {
                    // no skinning influences, just copy the values
                    positions[v] = vtxPos;
                    normals[v] = normal;
                }
            }
        }
    }

    // initialize the mesh deformer
    void DualQuatSkinDeformer::Reinitialize(Actor* actor, Node* node, uint32 lodLevel)
    {
        MCORE_UNUSED(actor);
        MCORE_UNUSED(node);
        MCORE_UNUSED(lodLevel);

        // clear the bone information array, but don't free the currently allocated/reserved memory
        m_bones.clear();

        // if there is no mesh
        if (mMesh == nullptr)
        {
            return;
        }

        SkinningInfoVertexAttributeLayer* skinningLayer = (SkinningInfoVertexAttributeLayer*)mMesh->FindSharedVertexAttributeLayer(SkinningInfoVertexAttributeLayer::TYPE_ID);
        MCORE_ASSERT(skinningLayer);

        // find out what bones this mesh uses
        const uint32 numOrgVerts = mMesh->GetNumOrgVertices();
        for (uint32 i = 0; i < numOrgVerts; i++)
        {
            // now we have located the skinning information for this vertex, we can see if our bones array
            // already contains the bone it uses by traversing all influences for this vertex, and checking
            // if the bone of that influence already is in the array with used bones
            const size_t numInfluences = skinningLayer->GetNumInfluences(i);
            for (size_t a = 0; a < numInfluences; ++a)
            {
                SkinInfluence* influence = skinningLayer->GetInfluence(i, a);

                AZ::Outcome<size_t> boneIndexOutcome = FindLocalBoneIndex(influence->GetNodeNr());
                if (boneIndexOutcome.IsSuccess())
                {
                    influence->SetBoneNr(boneIndexOutcome.GetValue());
                }
                else
                {
                    // add the bone to the array of bones in this deformer
                    BoneInfo lastBone;
                    lastBone.mNodeNr = influence->GetNodeNr();
                    lastBone.mDualQuat.Identity();
                    m_bones.emplace_back(lastBone);
                    influence->SetBoneNr(static_cast<AZ::u16>(m_bones.size() - 1));
                }
            }
        }
    }
} // namespace EMotionFX

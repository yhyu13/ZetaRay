#include "glTF.h"
#include "glTFAsset.h"
#include "../Math/MatrixFuncs.h"
#include "../Math/Surface.h"
#include "../Math/Quaternion.h"
#include "../Scene/SceneCore.h"
#include "../RayTracing/RtCommon.h"
#include "../Support/Task.h"
#include "../Core/RendererCore.h"
#include "../Core/GpuMemory.h"
#include "../App/Log.h"
#include "../Support/ThreadSafeMemoryArena.h"
#include "../Utility/Utility.h"
#include <algorithm>

#define CGLTF_IMPLEMENTATION
#include <cgltf/cgltf.h>

using namespace ZetaRay;
using namespace ZetaRay::Scene;
using namespace ZetaRay::Math;
using namespace ZetaRay::Core;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;
using namespace ZetaRay::Model;
using namespace ZetaRay::App;
using namespace ZetaRay::Model::glTF::Asset;
using namespace ZetaRay::Core::Direct3DHelper;

//--------------------------------------------------------------------------------------
// glTF
//--------------------------------------------------------------------------------------

namespace
{
	ZetaInline const char* GetErrorMsg(cgltf_result r) noexcept
	{
		switch (r)
		{
		case cgltf_result_data_too_short:
			return "cgltf_result_data_too_short";
		case cgltf_result_unknown_format:
			return "cgltf_result_unknown_format";
		case cgltf_result_invalid_json:
			return "cgltf_result_invalid_json";
		case cgltf_result_invalid_gltf:
			return "cgltf_result_invalid_gltf";
		case cgltf_result_invalid_options:
			return "cgltf_result_invalid_options";
		case cgltf_result_file_not_found:
			return "cgltf_result_file_not_found";
		case cgltf_result_io_error:
			return "cgltf_result_io_error";
		case cgltf_result_out_of_memory:
			return "cgltf_result_out_of_memory";
		case cgltf_result_legacy_gltf:
			return "cgltf_result_legacy_gltf";
		default:
			return "unknown error";
		}
	}

#ifndef Checkgltf
#define Checkgltf(expr)																											  \
	{																														      \
		cgltf_result r = (expr);																								  \
		if (r != cgltf_result_success)                                                                                            \
		{                                                                                                                         \
			char buff_[256];                                                                                                      \
			int n_ = stbsp_snprintf(buff_, 256, "cgltf call failed at %s: %d\nError: %s", __FILE__, __LINE__, GetErrorMsg(r));    \
			ZetaRay::Util::ReportError("Fatal Error", buff_);                                                                     \
			ZetaRay::Util::DebugBreak();                                                                                          \
		}                                                                                                                         \
	}
#endif
	
	void ProcessPositions(const cgltf_data& model, const cgltf_accessor& accessor, Span<Vertex> vertices, uint32_t baseOffset) noexcept
	{
		Check(accessor.type == cgltf_type_vec3, "Invalid type for POSITION attribute.");
		Check(accessor.component_type == cgltf_component_type_r_32f,
			"Invalid component type for POSITION attribute.");

		const cgltf_buffer_view& bufferView = *accessor.buffer_view;
		Check(accessor.stride == sizeof(float3), "Invalid stride for POSITION attribute.");

		const cgltf_buffer& buffer = *bufferView.buffer;
		const float3* start = reinterpret_cast<float3*>(reinterpret_cast<uintptr_t>(buffer.data) + bufferView.offset + accessor.offset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float3* curr = start + i;

			// glTF uses a right-handed coordinate system with +Y as up
			vertices[baseOffset + i].Position = float3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessNormals(const cgltf_data& model, const cgltf_accessor& accessor, Span<Vertex> vertices, uint32_t baseOffset) noexcept
	{
		Check(accessor.type == cgltf_type_vec3, "Invalid type for NORMAL attribute.");
		Check(accessor.component_type == cgltf_component_type_r_32f,
			"Invalid component type for NORMAL attribute.");

		const cgltf_buffer_view& bufferView = *accessor.buffer_view;
		Check(accessor.stride == sizeof(float3), "Invalid stride for NORMAL attribute.");

		const cgltf_buffer& buffer = *bufferView.buffer;
		const float3* start = reinterpret_cast<float3*>(reinterpret_cast<uintptr_t>(buffer.data) + bufferView.offset + accessor.offset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float3* curr = start + i;

			// glTF uses a right-handed coordinate system with +Y as up
			vertices[baseOffset + i].Normal = half3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessTexCoords(const cgltf_data& model, const cgltf_accessor& accessor, Span<Vertex> vertices, uint32_t baseOffset) noexcept
	{
		Check(accessor.type == cgltf_type_vec2, "Invalid type for TEXCOORD_0 attribute.");
		Check(accessor.component_type == cgltf_component_type_r_32f,
			"Invalid component type for TEXCOORD_0 attribute.");

		const cgltf_buffer_view& bufferView = *accessor.buffer_view;
		Check(accessor.stride == sizeof(float2), "Invalid stride for TEXCOORD_0 attribute.");

		const cgltf_buffer& buffer = *bufferView.buffer;
		const float2* start = reinterpret_cast<float2*>(reinterpret_cast<uintptr_t>(buffer.data) + bufferView.offset + accessor.offset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float2* curr = start + i;
			vertices[baseOffset + i].TexUV = float2(curr->x, curr->y);
		}
	}

	void ProcessTangents(const cgltf_data& model, const cgltf_accessor& accessor, Span<Vertex> vertices, uint32_t baseOffset) noexcept
	{
		Check(accessor.type == cgltf_type_vec4, "Invalid type for TANGENT attribute.");
		Check(accessor.component_type == cgltf_component_type_r_32f,
			"Invalid component type for TANGENT attribute.");

		const cgltf_buffer_view& bufferView = *accessor.buffer_view;

		const cgltf_buffer& buffer = *bufferView.buffer;
		const float4* start = reinterpret_cast<float4*>(reinterpret_cast<uintptr_t>(buffer.data) + bufferView.offset + accessor.offset);

		for (size_t i = 0; i < accessor.count; i++)
		{
			const float4* curr = start + i;

			// glTF uses a right-handed coordinate system with +Y as up
			vertices[baseOffset + i].Tangent = half3(curr->x, curr->y, -curr->z);
		}
	}

	void ProcessIndices(const cgltf_data& model, const cgltf_accessor& accessor, Span<uint32_t> indices, uint32_t baseOffset) noexcept
	{
		Check(accessor.type == cgltf_type_scalar, "Invalid index type.");
		Check(accessor.stride != -1, "Invalid index stride.");
		Check(accessor.count % 3 == 0, "invalid number of indices");

		const cgltf_buffer_view& bufferView = *accessor.buffer_view;
		const cgltf_buffer& buffer = *bufferView.buffer;

		// populate the mesh indices
		uint8_t* curr = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(buffer.data) + bufferView.offset + accessor.offset);
		const size_t numFaces = accessor.count / 3;
		const size_t indexStrideInBytes = accessor.stride;
		size_t currIdxOffset = 0;

		for (size_t face = 0; face < numFaces; face++)
		{
			uint32_t i0 = 0;
			uint32_t i1 = 0;
			uint32_t i2 = 0;

			memcpy(&i0, curr, indexStrideInBytes);
			curr += indexStrideInBytes;
			memcpy(&i1, curr, indexStrideInBytes);
			curr += indexStrideInBytes;
			memcpy(&i2, curr, indexStrideInBytes);
			curr += indexStrideInBytes;

			// use a clockwise ordering
			indices[baseOffset + currIdxOffset++] = i0;
			indices[baseOffset + currIdxOffset++] = i2;
			indices[baseOffset + currIdxOffset++] = i1;
		}
	}

	void ProcessMeshes(const cgltf_data& model, size_t offset, size_t size, 
		Span<Vertex> vertices, std::atomic_uint32_t& vertexCounter,
		Span<uint32_t> indices, std::atomic_uint32_t& idxCounter,
		Span<MeshSubset> meshPrims, std::atomic_uint32_t& meshPrimCounter) noexcept
	{
		SceneCore& scene = App::GetScene();

		uint32_t totalMeshPrims = 0;
		uint32_t totalVertices = 0;
		uint32_t totalIndices = 0;

		// figure out total number of mesh prims, vertices and indices
		for (size_t meshIdx = offset; meshIdx != offset + size; meshIdx++)
		{
			Assert(meshIdx < model.meshes_count, "out-of-bound access");
			const cgltf_mesh& mesh = model.meshes[meshIdx];

			for (int primIdx = 0; primIdx < mesh.primitives_count; primIdx++)
			{
				const cgltf_primitive& prim = mesh.primitives[primIdx];

				Check(prim.indices->count > 0, "index buffer is required.");
				Check(prim.type == cgltf_primitive_type_triangles, "Non-triangle meshes are not supported.");

				int posIt = -1;

				for (int attrib = 0; attrib < prim.attributes_count; attrib++)
				{
					if (strcmp(prim.attributes[attrib].name, "POSITION") == 0)
					{
						posIt = attrib;
						break;
					}
				}

				Check(posIt != -1, "POSITION was not found in the vertex attributes.");

				const cgltf_accessor& accessor = *prim.attributes[posIt].data;
				const uint32_t numVertices = (uint32_t)accessor.count;
				totalVertices += numVertices;

				const uint32_t numIndices = (uint32_t)prim.indices->count;
				totalIndices += numIndices;
			}

			totalMeshPrims += (uint32_t)mesh.primitives_count;
		}

		// (sub)allocate
		const uint32_t workerBaseVtx = vertexCounter.fetch_add(totalVertices, std::memory_order_relaxed);
		const uint32_t workerBaseIdx = idxCounter.fetch_add(totalIndices, std::memory_order_relaxed);
		const uint32_t workerBaseMeshPrim = meshPrimCounter.fetch_add(totalMeshPrims, std::memory_order_relaxed);

		uint32_t currVtxOffset = workerBaseVtx;
		uint32_t currIdxOffset = workerBaseIdx;
		uint32_t currMeshPrimOffset = workerBaseMeshPrim;

		// now iterate again and populate the buffers
		for (size_t meshIdx = offset; meshIdx != offset + size; meshIdx++)
		{
			const cgltf_mesh& mesh = model.meshes[meshIdx];

			// fill in the subsets
			for (int primIdx = 0; primIdx < mesh.primitives_count; primIdx++)
			{
				const cgltf_primitive& prim = mesh.primitives[primIdx];
				
				int posIt = -1;
				int normalIt = -1;
				int texIt = -1;
				int tangentIt = -1;

				for (int attrib = 0; attrib < prim.attributes_count; attrib++)
				{
					if(strcmp(prim.attributes[attrib].name, "POSITION") == 0)
						posIt = attrib;
					else if (strcmp(prim.attributes[attrib].name, "NORMAL") == 0)
						normalIt = attrib;
					else if (strcmp(prim.attributes[attrib].name, "TEXCOORD_0") == 0)
						texIt = attrib;
					else if (strcmp(prim.attributes[attrib].name, "TANGENT") == 0)
						tangentIt = attrib;
				}
				
				Check(normalIt != -1, "NORMAL was not found in the vertex attributes.");

				// populate the vertex attributes
				const cgltf_accessor& accessor = *prim.attributes[posIt].data;
				const uint32_t numVertices = (uint32_t)accessor.count;

				const cgltf_buffer_view& bufferView = *prim.indices->buffer_view;
				const uint32_t numIndices = (uint32_t)prim.indices->count;

				// POSITION
				ProcessPositions(model, *prim.attributes[posIt].data, vertices, currVtxOffset);

				// NORMAL
				ProcessNormals(model, *prim.attributes[normalIt].data, vertices, currVtxOffset);

				// indices
				ProcessIndices(model, *prim.indices, indices, currIdxOffset);

				// TEXCOORD_0
				if (texIt != -1)
				{
					ProcessTexCoords(model, *prim.attributes[texIt].data, vertices, currVtxOffset);

					// if vertex tangents aren't present, compute them. Make sure the computation happens after 
					// vertex & index processing
					if (tangentIt != -1)
						ProcessTangents(model, *prim.attributes[tangentIt].data, vertices, currVtxOffset);
					else
					{
						Math::ComputeMeshTangentVectors(Span(vertices.begin() + currVtxOffset, numVertices),
							Span(indices.begin() + currIdxOffset, numIndices),
							false);
					}
				}

				meshPrims[currMeshPrimOffset++] = MeshSubset
				{
					.MaterialIdx = prim.material ? (int)(prim.material - model.materials) : -1,
					.MeshIdx = (int)meshIdx,
					.MeshPrimIdx = primIdx,
					.BaseVtxOffset = currVtxOffset,
					.BaseIdxOffset = currIdxOffset,
					.NumVertices = numVertices,
					.NumIndices = numIndices
				};

				currVtxOffset += numVertices;
				currIdxOffset += numIndices;
			}
		}
	}

	void LoadDDSImages(uint64_t sceneID, const Filesystem::Path& modelDir, const cgltf_data& model,
		size_t offset, size_t size, Span<DDSImage> ddsImages) noexcept
	{
		char ext[8];

		for (size_t m = offset; m != offset + size; m++)
		{
			const cgltf_image& image = model.images[m];
			if (image.uri)
			{
				Filesystem::Path p(App::GetAssetDir());
				p.Append(modelDir.GetView());
				p.Append(image.uri);
				p.Extension(ext);

				if (strcmp(ext, "dds") != 0)
					continue;

				const uint64_t id = XXH3_64bits(p.Get(), p.Length());
				Texture tex;
				auto err = App::GetRenderer().GetGpuMemory().GetTexture2DFromDisk(p.Get(), tex);

				if (err != LOAD_DDS_RESULT::SUCCESS)
				{
					if (err == LOAD_DDS_RESULT::FILE_NOT_FOUND)
					{
						LOG_UI_WARNING("Texture in path %s was present in the glTF scene, but no textures referred to it, skipping...\n", p.Get());
						continue;
					}
					else
						Check(false, "Error while loading DDS texture in path %s: %d", p.Get(), err);
				}

				ddsImages[m] = DDSImage{ .T = ZetaMove(tex), .ID = id };
			}
		}
	}

	void ProcessMaterials(uint64_t sceneID, const Filesystem::Path& modelDir, const cgltf_data& model,
		int offset, int size, Span<DDSImage> ddsImages) noexcept
	{
		auto getAlphaMode = [](cgltf_alpha_mode m) noexcept
		{
			switch (m)
			{
			case cgltf_alpha_mode_opaque:
				return Material::ALPHA_MODE::OPAQUE_;
			case cgltf_alpha_mode_mask:
				return Material::ALPHA_MODE::MASK;
			case cgltf_alpha_mode_blend:
				return Material::ALPHA_MODE::BLEND;
			default:
				break;
			}
			
			Assert(false, "invalid alpha mode.");
			return Material::ALPHA_MODE::OPAQUE_;
		};

		for (int m = offset; m != offset + size; m++)
		{
			const auto& mat = model.materials[m];
			Check(mat.has_pbr_metallic_roughness, "material is not supported.");

			glTF::Asset::MaterialDesc desc;

			desc.Index = m;
			desc.AlphaMode = getAlphaMode(mat.alpha_mode);
			desc.AlphaCuttoff = (float)mat.alpha_cutoff;
			desc.DoubleSided = mat.double_sided;

			// base color map
			{
				const cgltf_texture_view& baseColView = mat.pbr_metallic_roughness.base_color_texture;
				if (baseColView.texture)
				{
					Check(baseColView.texture->image, "textureView doesn't point to any image.");

					Filesystem::Path p(App::GetAssetDir());
					p.Append(modelDir.GetView());
					p.Append(baseColView.texture->image->uri);

					desc.BaseColorTexPath = XXH3_64bits(p.Get(), p.Length());
				}

				auto& f = mat.pbr_metallic_roughness.base_color_factor;
				desc.BaseColorFactor = float4(f[0], f[1], f[2], f[3]);
			}

			// normal map
			{
				const cgltf_texture_view& normalView = mat.normal_texture;
				if (normalView.texture)
				{
					Check(normalView.texture->image, "textureView doesn't point to any image.");
					const char* texPath = normalView.texture->image->uri;

					Filesystem::Path p(App::GetAssetDir());
					p.Append(modelDir.GetView());
					p.Append(normalView.texture->image->uri);
					desc.NormalTexPath = XXH3_64bits(p.Get(), p.Length());

					desc.NormalScale = (float)mat.normal_texture.scale;
				}
			}

			// metalness-roughness map
			{
				const cgltf_texture_view& metalnessRoughnessView = mat.pbr_metallic_roughness.metallic_roughness_texture;
				if (metalnessRoughnessView.texture)
				{
					Check(metalnessRoughnessView.texture->image, "textureView doesn't point to any image.");

					Filesystem::Path p(App::GetAssetDir());
					p.Append(modelDir.GetView());
					p.Append(metalnessRoughnessView.texture->image->uri);
					desc.MetalnessRoughnessTexPath = XXH3_64bits(p.Get(), p.Length());
				}

				desc.MetalnessFactor = (float)mat.pbr_metallic_roughness.metallic_factor;
				desc.RoughnessFactor = (float)mat.pbr_metallic_roughness.roughness_factor;
			}

			// emissive map
			{
				const cgltf_texture_view& emissiveView = mat.emissive_texture;
				if (emissiveView.texture)
				{
					Check(emissiveView.texture->image, "textureView doesn't point to any image.");
					const char* texPath = emissiveView.texture->image->uri;

					Filesystem::Path p(App::GetAssetDir());
					p.Append(modelDir.GetView());
					p.Append(emissiveView.texture->image->uri);
					desc.EmissiveTexPath = XXH3_64bits(p.Get(), p.Length());
				}

				auto& f = mat.emissive_factor;
				desc.EmissiveFactor = float3((float)f[0], (float)f[1], (float)f[2]);

				if (mat.has_emissive_strength)
					desc.EmissiveStrength = mat.emissive_strength.emissive_strength;
			}

			SceneCore& scene = App::GetScene();
			scene.AddMaterial(sceneID, desc, ddsImages);
		}
	}

	void ProcessNodeSubtree(const cgltf_node& node, uint64_t sceneID, const cgltf_data& model, uint64_t parentId) noexcept
	{
		uint64_t currInstanceID = SceneCore::ROOT_ID;

		AffineTransformation transform = AffineTransformation::GetIdentity();

		if (node.has_matrix)
		{
			float4x4a M(node.matrix);
			v_float4x4 vM = load(M);
			auto det = store(det3x3(vM));	// last column/row is ignored
			//Check(fabsf(det.x) > 1e-6f, "Transformation matrix with a zero determinant is invalid.");
			Check(det.x > 0.0f, "Transformation matrices that change the orientation (e.g. negative scaling) are not supported.");
			
			// column-major storage to row-major storage
			vM = transpose(vM);
			M = store(vM);

			// RHS transformation matrix M_rhs can be converted to LHS (+Y up) as follows:
			//
			//		transform = M_RhsToLhs * M_rhs * M_LhsToRhs
			// 
			// where M_RhsToLhs is a change-of-basis transformation matrix and M_LhsToRhs = M_RhsToLhs^-1. 
			// Replacing in above:
			//
			//                  | 1 0  0 |             | 1 0  0 |
			//		transform = | 0 1  0 | * [u v w] * | 0 1  0 |
			//                  | 0 0 -1 |             | 0 0 -1 |
			//
			//                  | 1 0  0 |                  
			//                = | 0 1  0 | * [u v -w]
			//                  | 0 0 -1 |
			//
			//                  |  u_1  v_1  -w_1 |                  
			//                = |  u_2  v_2  -w_2 |
			//                  | -u_3 -v_3   w_3 |
			M.m[0].z *= -1.0f;
			M.m[1].z *= -1.0f;
			M.m[2].x *= -1.0f;
			M.m[2].y *= -1.0f;

			// convert translation to LHS
			M.m[2].w *= -1.0f;

			vM = load(M);
			decomposeTRS(vM, transform.Scale, transform.Rotation, transform.Translation);
		}
		else
		{
			if (node.has_scale)
			{
				Check(node.scale[0] > 0 && node.scale[1] > 0 && node.scale[2] > 0, "Negative or zero scale factors are not supported.");
				transform.Scale = float3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);
			}

			if (node.has_translation)
				transform.Translation = float3((float)node.translation[0], (float)node.translation[1], (float)-node.translation[2]);

			if (node.has_rotation)
			{
				// rotation quaternion = (n_x * s, n_y * s, n_z * s, c)
				// where s = sin(theta/2) and c = cos(theta/2)
				//
				// In the left-handed coord. system here with +Y as up, n_lhs = (n_x, n_y, -n_z)
				// and theta_lhs = -theta. Since sin(-a) = -sin(a) and cos(-a) = cos(a) we have:
				//
				//		q_lhs = (n_x * -s, n_y * -s, -n_z * -s, c)
				//			  = (-n_x * s, -n_y * s, n_z * s, c)
				//
				transform.Rotation = float4(-(float)node.rotation[0], 
					-(float)node.rotation[1],
					(float)node.rotation[2], 
					(float)node.rotation[3]);

				// check ||quaternion|| = 1
				__m128 vV = _mm_loadu_ps(&transform.Rotation.x);
				__m128 vLength = _mm_dp_ps(vV, vV, 0xff);
				vLength = _mm_sqrt_ps(vLength);
				__m128 vOne = _mm_set1_ps(1.0f);
				__m128 vDiff = _mm_sub_ps(vLength, vOne);
				float d = _mm_cvtss_f32(abs(vDiff));
				Check(d < 1e-6f, "Invalid rotation quaternion.");
			}
		}

		// workaround for nodes without a name
		const int nodeIdx = (int)(&node - model.nodes);
		Assert(nodeIdx < model.nodes_count, "invalid node index.");
		char nodeIdxStr[4] = {};
		stbsp_snprintf(nodeIdxStr, sizeof(nodeIdx), "%d", nodeIdx);
		const char* instanceName = node.name ? node.name : nodeIdxStr;

		if (node.mesh)
		{
			const int meshIdx = (int)(node.mesh - model.meshes);
			Assert(meshIdx < model.meshes_count, "invalid mesh index.");

			// a seperate instance for each primitive
			for (int primIdx = 0; primIdx < node.mesh->primitives_count; primIdx++)
			{
				const cgltf_primitive& meshPrim = node.mesh->primitives[primIdx];

				float oneDotEmissvieFactor = meshPrim.material->emissive_factor[0];
				oneDotEmissvieFactor += meshPrim.material->emissive_factor[1];
				oneDotEmissvieFactor += meshPrim.material->emissive_factor[2];

				uint8_t rtInsMask = meshPrim.material && 
					(meshPrim.material->emissive_texture.texture || oneDotEmissvieFactor > 1e-4f)?
					RT_AS_SUBGROUP::EMISSIVE : RT_AS_SUBGROUP::NON_EMISSIVE;

				// parent-child relationships will be w.r.t. the last mesh primitive
				currInstanceID = SceneCore::InstanceID(sceneID, instanceName, meshIdx, primIdx);

				glTF::Asset::InstanceDesc desc{
					.LocalTransform = transform,
					.MeshIdx = meshIdx,
					.ID = currInstanceID,
					.ParentID = parentId,
					.MeshPrimIdx = primIdx,
					.RtMeshMode = RT_MESH_MODE::STATIC,
					.RtInstanceMask = rtInsMask };

				SceneCore& scene = App::GetScene();
				scene.AddInstance(sceneID, ZetaMove(desc));
			}
		}
		else
		{
			currInstanceID = SceneCore::InstanceID(sceneID, instanceName, -1, -1);

			glTF::Asset::InstanceDesc desc{
				.LocalTransform = transform,
					.MeshIdx = -1,
					.ID = currInstanceID,
					.ParentID = parentId,
					.MeshPrimIdx = -1,
					.RtMeshMode = RT_MESH_MODE::STATIC,
					.RtInstanceMask = RT_AS_SUBGROUP::NON_EMISSIVE };

			SceneCore& scene = App::GetScene();
			scene.AddInstance(sceneID, ZetaMove(desc));
		}

		for (int c = 0; c < node.children_count; c++)
		{
			const cgltf_node& childNode = *node.children[c];
			ProcessNodeSubtree(childNode, sceneID, model, currInstanceID);
		}
	}

	void ProcessNodes(const cgltf_data& model, uint64_t sceneID) noexcept
	{
		for (size_t i = 0; i < model.scene->nodes_count; i++)
		{
			const cgltf_node& node = *model.scene->nodes[i];
			ProcessNodeSubtree(node, sceneID, model, SceneCore::ROOT_ID);
		}
	}

	void TotalNumVerticesAndIndices(cgltf_data* model, size_t& numVertices, size_t& numIndices, size_t& numMeshes) noexcept
	{
		numVertices = 0;
		numIndices = 0;
		numMeshes = 0;

		for (size_t meshIdx = 0; meshIdx != model->meshes_count; meshIdx++)
		{
			const auto& mesh = model->meshes[meshIdx];
			numMeshes += mesh.primitives_count;

			for(size_t primIdx = 0; primIdx < mesh.primitives_count; primIdx++)
			{
				const auto& prim = mesh.primitives[primIdx];

				if (prim.type != cgltf_primitive_type_triangles)
					continue;

				for (int attrib = 0; attrib < prim.attributes_count; attrib++)
				{
					if (strcmp("POSITION", prim.attributes[attrib].name) == 0)
					{
						auto& accessor = prim.attributes[attrib].data;
						numVertices += accessor->count;

						break;
					}
				}

				numIndices += prim.indices->count;
			}
		}
	}
}

void glTF::Load(const App::Filesystem::Path& pathToglTF) noexcept
{
	// parse json
	cgltf_options options{};
	cgltf_data* model = nullptr;
	Checkgltf(cgltf_parse_file(&options, pathToglTF.GetView().data(), &model));

	Check(model->extensions_required_count == 0, "Required glTF extensions are not supported.");

	// load buffers
	Check(model->buffers_count == 1, "invalid number of buffers");
	Filesystem::Path bufferPath(pathToglTF.GetView());
	bufferPath.Directory();
	bufferPath.Append(model->buffers[0].uri);
	Checkgltf(cgltf_load_buffers(&options, model, bufferPath.Get()));

	Check(model->scene, "no scene found in glTF file: %s.", pathToglTF.GetView());
	const uint64_t sceneID = XXH3_64bits(pathToglTF.GetView().data(), pathToglTF.Length());
	SceneCore& scene = App::GetScene();

	// all the unique textures that need to be loaded from disk
	SmallVector<DDSImage> ddsImages;
	ddsImages.resize(model->images_count);

	// figure out total number of vertices & indices
	size_t totalNumVertices;
	size_t totalNumIndices;
	size_t totalNumMeshPrims;
	TotalNumVerticesAndIndices(model, totalNumVertices, totalNumIndices, totalNumMeshPrims);

	// preallocate
	Util::SmallVector<Core::Vertex> vertices;
	Util::SmallVector<uint32_t> indices;
	Util::SmallVector<MeshSubset> meshPrims;

	vertices.resize(totalNumVertices);
	indices.resize(totalNumIndices);
	meshPrims.resize(totalNumMeshPrims);

	// how many meshes are processed by each worker
	constexpr size_t MAX_NUM_MESH_WORKERS = 4;
	constexpr size_t MIN_MESHES_PER_WORKER = 20;
	size_t meshThreadOffsets[MAX_NUM_MESH_WORKERS];
	size_t meshThreadSizes[MAX_NUM_MESH_WORKERS];

	const size_t meshNumThreads = SubdivideRangeWithMin(model->meshes_count,
		MAX_NUM_MESH_WORKERS,
		meshThreadOffsets,
		meshThreadSizes,
		MIN_MESHES_PER_WORKER);

	// how many images are processed by each worker
	constexpr size_t MAX_NUM_IMAGE_WORKERS = 5;
	constexpr size_t MIN_IMAGES_PER_WORKER = 15;
	size_t imgThreadOffsets[MAX_NUM_IMAGE_WORKERS];
	size_t imgThreadSizes[MAX_NUM_IMAGE_WORKERS];

	const size_t imgNumThreads = SubdivideRangeWithMin(model->images_count,
		MAX_NUM_IMAGE_WORKERS,
		imgThreadOffsets,
		imgThreadSizes,
		MIN_IMAGES_PER_WORKER);

	// how many materials are processed by each worker
	constexpr size_t MAX_NUM_MAT_WORKERS = 1;
	constexpr size_t MIN_MATS_PER_WORKER = 20;
	size_t matThreadOffsets[MAX_NUM_MAT_WORKERS];
	size_t matThreadSizes[MAX_NUM_MAT_WORKERS];

	const size_t matNumThreads = SubdivideRangeWithMin(model->materials_count,
		MAX_NUM_MAT_WORKERS,
		matThreadOffsets,
		matThreadSizes,
		MIN_MATS_PER_WORKER);

	std::atomic_uint32_t currVtxOffset = 0;
	std::atomic_uint32_t currIdxOffset = 0;
	std::atomic_uint32_t currMeshPrimOffset = 0;

	struct ThreadContext
	{
		uint64_t SceneID;
		cgltf_data* Model;
		size_t* MeshThreadOffsets;
		size_t* MeshThreadSizes;
		size_t* MatThreadOffsets;
		size_t* MatThreadSizes;
		size_t* ImgThreadOffsets;
		size_t* ImgThreadSizes;
		Span<Vertex> Vertices;
		std::atomic_uint32_t& CurrVtxOffset;
		Span<uint32_t> Indices;
		std::atomic_uint32_t& CurrIdxOffset;
		Span<MeshSubset> MeshPrims;
		std::atomic_uint32_t& CurrMeshPrimOffset;
	};

	ThreadContext tc{ .SceneID = sceneID, .Model = model,
		.MeshThreadOffsets = meshThreadOffsets, .MeshThreadSizes = meshThreadSizes,
		.MatThreadOffsets = matThreadOffsets, .MatThreadSizes = matThreadSizes,
		.ImgThreadOffsets = imgThreadOffsets, .ImgThreadSizes = imgThreadSizes,
		.Vertices = vertices,
		.CurrVtxOffset = currVtxOffset,
		.Indices = indices,
		.CurrIdxOffset = currIdxOffset,
		.MeshPrims = meshPrims,
		.CurrMeshPrimOffset = currMeshPrimOffset };

	TaskSet ts;

	auto addMeshesToScene = ts.EmplaceTask("AddMeshesToScene", [&meshPrims, &vertices, &indices, sceneID]()
		{
			SceneCore& scene = App::GetScene();
			scene.AddMeshes(sceneID, ZetaMove(meshPrims), ZetaMove(vertices), ZetaMove(indices));
		});

	for (size_t i = 0; i < meshNumThreads; i++)
	{
		StackStr(tname, n, "gltf::ProcessMesh_%d", i);

		auto h = ts.EmplaceTask(tname, [&tc, rangeIdx = i]()
			{
				ProcessMeshes(*tc.Model, tc.MeshThreadOffsets[rangeIdx], tc.MeshThreadSizes[rangeIdx], 
					tc.Vertices, tc.CurrVtxOffset, 
					tc.Indices, tc.CurrIdxOffset,
					tc.MeshPrims, tc.CurrMeshPrimOffset);
			});

		ts.AddOutgoingEdge(h, addMeshesToScene);
	}

	auto sortTask = ts.EmplaceTask("gltf::Sort", [&ddsImages]()
		{
			std::sort(ddsImages.begin(), ddsImages.end(), [](const DDSImage& lhs, const DDSImage& rhs)
				{
					return lhs.ID < rhs.ID;
				});
		});

	for (size_t i = 0; i < imgNumThreads; i++)
	{
		StackStr(tname, n, "gltf::ProcessImg_%d", i);
		Assert(i < MAX_NUM_IMAGE_WORKERS, "invalid index.");

		auto h = ts.EmplaceTask(tname, [&pathToglTF, &ddsImages, &tc, rangeIdx = i]()
			{
				Filesystem::Path parent(pathToglTF.GetView());
				parent.ToParent();

				LoadDDSImages(tc.SceneID, parent, *tc.Model, tc.ImgThreadOffsets[rangeIdx], tc.ImgThreadSizes[rangeIdx], ddsImages);
			});

		// sort after all images are loaded
		ts.AddOutgoingEdge(h, sortTask);
	}

	for (size_t i = 0; i < matNumThreads; i++)
	{
		StackStr(tname, n, "gltf::ProcessMats_%d", i);

		auto h = ts.EmplaceTask(tname, [&pathToglTF, &ddsImages, &tc, rangeIdx = i]()
			{
				Filesystem::Path parent(pathToglTF.GetView());
				parent.ToParent();

				ProcessMaterials(tc.SceneID, parent, *tc.Model, (int)tc.MatThreadOffsets[rangeIdx], (int)tc.MatThreadSizes[rangeIdx], ddsImages);
			});

		// make sure processing materials starts after textures are loaded
		ts.AddOutgoingEdge(sortTask, h);
	}

	WaitObject waitObj;
	ts.Sort();
	ts.Finalize(&waitObj);
	App::Submit(ZetaMove(ts));

	waitObj.Wait();

	ProcessNodes(*model, sceneID);

	cgltf_free(model);
}
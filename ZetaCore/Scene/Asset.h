#pragma once

#include "../Model/Mesh.h"
#include "../Core/Material.h"
#include "../Utility/HashTable.h"
#include "../Core/DescriptorHeap.h"
#include "../Core/GpuMemory.h"
#include "../Model/glTFAsset.h"

namespace ZetaRay::App::Filesystem
{
	struct Path;
}

namespace ZetaRay::Scene::Internal
{
	//--------------------------------------------------------------------------------------
	// TextureDescriptorTable: a descriptor table containing a contiguous set of textures, which
	// are to be bound as unbounded descriptor tables in the shaders. Each texture index in
	// a given Material refers to an offset in one such descriptor table
	//--------------------------------------------------------------------------------------

	struct TexSRVDescriptorTable
	{
		TexSRVDescriptorTable(const uint32_t descTableSize = 1024) noexcept;
		~TexSRVDescriptorTable() noexcept = default;

		TexSRVDescriptorTable(const TexSRVDescriptorTable&) = delete;
		TexSRVDescriptorTable& operator=(const TexSRVDescriptorTable&) = delete;

		void Init(uint64_t id) noexcept;

		// Returns offset of the given texture in the desc. table. The texture is then loaded from
		// the disk. "id" is hash of the texture path
		uint32_t Add(Core::Texture&& tex, uint64_t id) noexcept;
		//void Remove(uint64_t id, uint64_t nextFenceVal) noexcept;

		void Clear() noexcept;
		void Recycle(uint64_t completedFenceVal) noexcept;

		struct ToBeFreedTexture
		{
			Core::Texture T;
			uint64_t FenceVal;
			uint32_t DescTableOffset;
		};

		Util::SmallVector<ToBeFreedTexture> m_pending;

		static constexpr int MAX_NUM_DESCRIPTORS = 1024;
		static constexpr int MAX_NUM_MASKS = MAX_NUM_DESCRIPTORS >> 6;
		static_assert(MAX_NUM_MASKS * 64 == MAX_NUM_DESCRIPTORS, "these must match.");
		
		const uint32_t m_descTableSize;
		const uint32_t m_numMasks;
		uint64_t m_inUseBitset[MAX_NUM_MASKS] = { 0 };

		struct CacheEntry
		{
			Core::Texture T;
			uint32_t DescTableOffset = uint32_t(-1);
			uint32_t RefCount = 0;
		};

		Core::DescriptorTable m_descTable;
		Util::HashTable<CacheEntry> m_cache;
	};

	//--------------------------------------------------------------------------------------
	// MaterialBuffer: a wrapper over an upload-heap buffer containing all the materials required
	// for the current frame
	//--------------------------------------------------------------------------------------

	struct MaterialBuffer
	{
		MaterialBuffer() noexcept = default;
		~MaterialBuffer() noexcept = default;

		MaterialBuffer(const MaterialBuffer&) = delete;
		MaterialBuffer& operator=(const MaterialBuffer&) = delete;

		void Init(uint64_t id) noexcept;

		// Allocates an entry for the given material. Index to the allocated entry is also set
		void Add(uint64_t id, Material& mat) noexcept;
		void UpdateGPUBufferIfStale() noexcept;
		//void Remove(uint64_t id, uint64_t nextFenceVal) noexcept;

		// returns a copy since references to elements are not stable
		ZetaInline Material Get(uint64_t id) noexcept
		{
			Material* m = m_matTable.find(id);
			Assert(m, "material with id %llu was not found", id);

			if (!m)
				return Material();

			return *m;
		}

		void Recycle(uint64_t completedFenceVal) noexcept;
		void Clear() noexcept;

		struct ToBeRemoved
		{
			uint64_t FenceVal;
			uint16_t Offset;
		};

		Util::SmallVector<ToBeRemoved> m_pending;

	private:
		static constexpr int MAX_NUM_MATERIALS = 2048;
		static constexpr int NUM_MASKS = MAX_NUM_MATERIALS >> 6;
		static_assert(NUM_MASKS * 64 == MAX_NUM_MATERIALS, "these must match.");
		uint64_t m_inUseBitset[NUM_MASKS] = { 0 };

		Core::DefaultHeapBuffer m_buffer;
			
		// references to elements are not stable
		Util::HashTable<Material> m_matTable;

		uint64_t k_bufferID = uint64_t (-1);
		bool m_stale = false;
	};

	//--------------------------------------------------------------------------------------
	// MeshContainer
	//--------------------------------------------------------------------------------------

	struct MeshContainer
	{
		void Add(uint64_t id, Util::Span<Core::Vertex> vertices, Util::Span<uint32_t> indices, uint64_t matID) noexcept;
		void AddBatch(uint64_t sceneID, Util::SmallVector<Model::glTF::Asset::MeshSubset>&& meshes, Util::SmallVector<Core::Vertex>&& vertices,
			Util::SmallVector<uint32_t>&& indices) noexcept;
		void Reserve(size_t numVertices, size_t numIndices) noexcept;
		void RebuildBuffers() noexcept;
		
		ZetaInline Model::TriangleMesh GetMesh(uint64_t id) noexcept
		{
			auto* mesh = m_meshes.find(id);
			Assert(mesh, "Mesh with id %llu was not found", id);

			return *mesh;
		}

		const Core::DefaultHeapBuffer& GetVB() { return m_vertexBuffer; }
		const Core::DefaultHeapBuffer& GetIB() { return m_indexBuffer; }

		void Clear() noexcept;

	private:
		Util::HashTable<Model::TriangleMesh> m_meshes;
		Util::SmallVector<Core::Vertex> m_vertices;
		Util::SmallVector<uint32_t> m_indices;

		Core::DefaultHeapBuffer m_vertexBuffer;
		Core::DefaultHeapBuffer m_indexBuffer;
	};
}
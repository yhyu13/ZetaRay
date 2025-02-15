// This implmentation uses a top-down approach to build the BVH. 
// 
// References:
// 1. Physically Based Rendering 3rd Ed.
// 2. Real-time Collision Detection

#pragma once

#include "../Utility/Span.h"
#include "../Math/CollisionTypes.h"
#include "../Support/MemoryArena.h"
#include "../App/App.h"

namespace ZetaRay::Math
{
	struct alignas(16) float4x4a;

	class BVH
	{
	public:
		struct alignas(16) BVHInput
		{
			Math::AABB AABB;
			uint64_t ID;
		};

		struct alignas(16) BVHUpdateInput
		{
			Math::AABB OldBox;
			Math::AABB NewBox;
			uint64_t ID;
		};

		BVH() noexcept;
		~BVH() noexcept = default;

		BVH(BVH&&) = delete;
		BVH& operator=(BVH&&) = delete;

		bool IsBuilt() noexcept { return m_nodes.size() != 0; }
		void Clear() noexcept;

		void Build(Util::Span<BVHInput> instances) noexcept;
		void Update(Util::Span<BVHUpdateInput> instances) noexcept;
		void Remove(uint64_t ID, const Math::AABB& AABB) noexcept;
		
		// Returns ID of instances that at least partially overlap the view frustum. Assumes 
		// the view frustum is in the view space
		void DoFrustumCulling(const Math::ViewFrustum& viewFrustum, 
			const Math::float4x4a& viewToWorld,
			Util::Vector<uint64_t, App::FrameAllocator>& visibleInstanceIDs);

		// Returns IDs & AABBs of instances that at least partially overlap the view frustum. Assumes 
		// the view frustum is in the view space
		void DoFrustumCulling(const Math::ViewFrustum& viewFrustum,
			const Math::float4x4a& viewToWorld,
			Util::Vector<BVHInput, App::FrameAllocator>& visibleInstanceIDs);

		// Casts a ray into the BVH and returns the closest-hit intersection. Given Ray has to 
		// be in world space
		uint64_t CastRay(Math::Ray& r) noexcept;

		// Returns the AABB that encompasses the scene
		Math::AABB GetWorldAABB() noexcept 
		{
			Assert(m_nodes.size() > 0, "BVH hasn't been built yet.");
			return m_nodes[0].AABB; 
		}

	private:
		// maximum number of instances that can be included in a leaf node
		static constexpr int MAX_NUM_INSTANCES_PER_LEAF = 8;
		static constexpr int MIN_NUM_INSTANCES_SPLIT_SAH = 10;
		static constexpr int NUM_SAH_BINS = 6;

		struct alignas(64) Node
		{
			bool IsInitialized() noexcept { return Parent != -1; }
			void InitAsLeaf(int base, int count, int parent) noexcept;
			void InitAsInternal(Util::Span<BVH::BVHInput> instances, int base, int count,
				int right, int parent) noexcept;
			bool IsLeaf() const { return RightChild == -1; }

			// Union AABB of all the child nodes for internal nodes
			// also used to distinguish between leaf & internal nodes
			Math::AABB AABB;

			/*
			union
			{
				struct
				{
					int Base;
					int Count;
				} Leaf;

				int RightChild;
			};
			*/

			// for leafs
			int Base;
			int Count;

			// for internal nodes
			int RightChild;

			int Parent = -1;
		};

		static constexpr int qfgh = sizeof(Node);

		// Recursively builds a BVH (subtree) for the given range
		int BuildSubtree(int base, int count, int parent) noexcept;

		// Finds the leaf node that contains the given instance. Returns -1 otherwise.
		int Find(uint64_t ID, const Math::AABB& AABB, int& modelIdx) noexcept;

		Support::MemoryArena m_arena;

		// tree hierarchy is stored as an array
		Util::SmallVector<Node, Support::ArenaAllocator> m_nodes;

		// array of inputs to build a BVH for. During BVH build, elements are moved around
		Util::SmallVector<BVHInput, Support::ArenaAllocator> m_instances;

		uint32_t m_numNodes = 0;
	};
}

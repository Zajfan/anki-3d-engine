// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Utils/Drawer.h>
#include <AnKi/Resource/ImageResource.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Util/Logger.h>
#include <AnKi/Shaders/Include/MaterialTypes.h>
#include <AnKi/Shaders/Include/GpuSceneFunctions.h>
#include <AnKi/Core/GpuMemory/UnifiedGeometryBuffer.h>
#include <AnKi/Core/GpuMemory/RebarTransientMemoryPool.h>
#include <AnKi/Core/GpuMemory/GpuSceneBuffer.h>
#include <AnKi/Core/StatsSet.h>
#include <AnKi/Scene/RenderStateBucket.h>

namespace anki {

RenderableDrawer::~RenderableDrawer()
{
}

Error RenderableDrawer::init()
{
	return Error::kNone;
}

void RenderableDrawer::setState(const RenderableDrawerArguments& args, CommandBuffer& cmdb)
{
	// Allocate, set and bind global uniforms
	{
		MaterialGlobalUniforms* globalUniforms;
		const RebarAllocation globalUniformsToken = RebarTransientMemoryPool::getSingleton().allocateFrame(1, globalUniforms);

		globalUniforms->m_viewProjectionMatrix = args.m_viewProjectionMatrix;
		globalUniforms->m_previousViewProjectionMatrix = args.m_previousViewProjectionMatrix;
		static_assert(sizeof(globalUniforms->m_viewTransform) == sizeof(args.m_viewMatrix));
		memcpy(&globalUniforms->m_viewTransform, &args.m_viewMatrix, sizeof(args.m_viewMatrix));
		static_assert(sizeof(globalUniforms->m_cameraTransform) == sizeof(args.m_cameraTransform));
		memcpy(&globalUniforms->m_cameraTransform, &args.m_cameraTransform, sizeof(args.m_cameraTransform));

		ANKI_ASSERT(args.m_viewport != UVec4(0u));
		globalUniforms->m_viewport = Vec4(args.m_viewport);

		globalUniforms->m_enableHzbTesting = args.m_hzbTexture.isValid();

		cmdb.bindUniformBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_GLOBAL_UNIFORMS), globalUniformsToken);
	}

	// More globals
	cmdb.bindSampler(ANKI_REG(ANKI_MATERIAL_REGISTER_TILINEAR_REPEAT_SAMPLER), args.m_sampler);
	cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_GPU_SCENE), GpuSceneBuffer::getSingleton().getBufferView());

#define ANKI_UNIFIED_GEOM_FORMAT(fmt, shaderType, reg) \
	cmdb.bindTexelBuffer( \
		ANKI_REG(reg), \
		BufferView(&UnifiedGeometryBuffer::getSingleton().getBuffer(), 0, \
				   getAlignedRoundDown(getFormatInfo(Format::k##fmt).m_texelSize, UnifiedGeometryBuffer::getSingleton().getBuffer().getSize())), \
		Format::k##fmt);
#include <AnKi/Shaders/Include/UnifiedGeometryTypes.def.h>

	cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_MESHLET_BOUNDING_VOLUMES), UnifiedGeometryBuffer::getSingleton().getBufferView());
	cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_MESHLET_GEOMETRY_DESCRIPTORS), UnifiedGeometryBuffer::getSingleton().getBufferView());
	if(args.m_mesh.m_meshletGroupInstancesBuffer.isValid())
	{
		cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_MESHLET_GROUPS), args.m_mesh.m_meshletGroupInstancesBuffer);
	}
	cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_RENDERABLES), GpuSceneArrays::Renderable::getSingleton().getBufferView());
	cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_MESH_LODS), GpuSceneArrays::MeshLod::getSingleton().getBufferView());
	cmdb.bindStorageBuffer(ANKI_REG(ANKI_MATERIAL_REGISTER_TRANSFORMS), GpuSceneArrays::Transform::getSingleton().getBufferView());
	cmdb.bindTexture(ANKI_REG(ANKI_MATERIAL_REGISTER_HZB_TEXTURE),
					 (args.m_hzbTexture.isValid()) ? args.m_hzbTexture
												   : TextureView(&getRenderer().getDummyTexture2d(), TextureSubresourceDesc::all()));
	cmdb.bindSampler(ANKI_REG(ANKI_MATERIAL_REGISTER_NEAREST_CLAMP_SAMPLER), getRenderer().getSamplers().m_nearestNearestClamp.get());

	// Misc
	cmdb.bindIndexBuffer(UnifiedGeometryBuffer::getSingleton().getBufferView(), IndexType::kU16);
}

void RenderableDrawer::drawMdi(const RenderableDrawerArguments& args, CommandBuffer& cmdb)
{
	ANKI_ASSERT(args.m_viewport != UVec4(0u));

	if(RenderStateBucketContainer::getSingleton().getBucketCount(args.m_renderingTechinuqe) == 0) [[unlikely]]
	{
		return;
	}

#if ANKI_STATS_ENABLED
	PipelineQueryPtr pplineQuery;

	if(GrManager::getSingleton().getDeviceCapabilities().m_pipelineQuery)
	{
		PipelineQueryInitInfo queryInit("Drawer");
		queryInit.m_type = PipelineQueryType::kPrimitivesPassedClipping;
		pplineQuery = GrManager::getSingleton().newPipelineQuery(queryInit);
		getRenderer().appendPipelineQuery(pplineQuery.get());

		cmdb.beginPipelineQuery(pplineQuery.get());
	}
#endif

	setState(args, cmdb);

	const Bool meshShaderHwSupport = GrManager::getSingleton().getDeviceCapabilities().m_meshShaders;

	cmdb.setVertexAttribute(VertexAttributeSemantic::kMisc0, 0, Format::kR32G32B32A32_Uint, 0);

	RenderStateBucketContainer::getSingleton().iterateBucketsPerformanceOrder(
		args.m_renderingTechinuqe,
		[&](const RenderStateInfo& state, U32 bucketIdx, U32 userCount, U32 meshletGroupCount, [[maybe_unused]] U32 meshletCount) {
			if(userCount == 0)
			{
				return;
			}

			cmdb.bindShaderProgram(state.m_program.get());

			const Bool meshlets = meshletGroupCount > 0;

			if(meshlets && meshShaderHwSupport)
			{
				const UVec4 firstPayload(args.m_mesh.m_bucketMeshletGroupInstanceRanges[bucketIdx].getFirstInstance());
				cmdb.setPushConstants(&firstPayload, sizeof(firstPayload));

				cmdb.drawMeshTasksIndirect(BufferView(
					&args.m_mesh.m_taskShaderIndirectArgsBuffer.getBuffer(),
					args.m_mesh.m_taskShaderIndirectArgsBuffer.getOffset() + sizeof(DispatchIndirectArgs) * bucketIdx, sizeof(DispatchIndirectArgs)));
			}
			else if(meshlets)
			{
				const InstanceRange& instanceRange = args.m_softwareMesh.m_bucketMeshletInstanceRanges[bucketIdx];
				const BufferView vertBufferView = BufferView(args.m_softwareMesh.m_meshletInstancesBuffer)
													  .incrementOffset(instanceRange.getFirstInstance() * sizeof(GpuSceneMeshletInstance))
													  .setRange(instanceRange.getInstanceCount() * sizeof(GpuSceneMeshletInstance));
				cmdb.bindVertexBuffer(0, vertBufferView, sizeof(GpuSceneMeshletInstance), VertexStepRate::kInstance);

				const BufferView indirectArgsBuffView = BufferView(args.m_softwareMesh.m_drawIndirectArgsBuffer)
															.incrementOffset(sizeof(DrawIndirectArgs) * bucketIdx)
															.setRange(sizeof(DrawIndirectArgs));
				cmdb.drawIndirect(PrimitiveTopology::kTriangles, indirectArgsBuffView);
			}
			else if(state.m_indexedDrawcall)
			{
				// Legacy

				const InstanceRange& instanceRange = args.m_legacy.m_bucketRenderableInstanceRanges[bucketIdx];
				const U32 maxDrawCount = instanceRange.getInstanceCount();

				const BufferView vertBufferView = BufferView(args.m_legacy.m_renderableInstancesBuffer)
													  .incrementOffset(instanceRange.getFirstInstance() * sizeof(GpuSceneRenderableInstance))
													  .setRange(instanceRange.getInstanceCount() * sizeof(GpuSceneRenderableInstance));
				cmdb.bindVertexBuffer(0, vertBufferView, sizeof(GpuSceneRenderableInstance), VertexStepRate::kInstance);

				const BufferView indirectArgsBuffView = BufferView(args.m_legacy.m_drawIndexedIndirectArgsBuffer)
															.incrementOffset(instanceRange.getFirstInstance() * sizeof(DrawIndexedIndirectArgs))
															.setRange(instanceRange.getInstanceCount() * sizeof(DrawIndexedIndirectArgs));
				const BufferView mdiCountBuffView =
					BufferView(args.m_legacy.m_mdiDrawCountsBuffer).incrementOffset(sizeof(U32) * bucketIdx).setRange(sizeof(U32));
				cmdb.drawIndexedIndirectCount(state.m_primitiveTopology, indirectArgsBuffView, sizeof(DrawIndexedIndirectArgs), mdiCountBuffView,
											  maxDrawCount);
			}
			else
			{
				// Legacy

				const InstanceRange& instanceRange = args.m_legacy.m_bucketRenderableInstanceRanges[bucketIdx];
				const U32 maxDrawCount = instanceRange.getInstanceCount();

				const BufferView vertBufferView = BufferView(args.m_legacy.m_renderableInstancesBuffer)
													  .incrementOffset(instanceRange.getFirstInstance() * sizeof(GpuSceneRenderableInstance))
													  .setRange(instanceRange.getInstanceCount() * sizeof(GpuSceneRenderableInstance));
				cmdb.bindVertexBuffer(0, vertBufferView, sizeof(GpuSceneRenderableInstance), VertexStepRate::kInstance);

				// Yes, the DrawIndexedIndirectArgs is intentional
				const BufferView indirectArgsBuffView = BufferView(args.m_legacy.m_drawIndexedIndirectArgsBuffer)
															.incrementOffset(instanceRange.getFirstInstance() * sizeof(DrawIndexedIndirectArgs))
															.setRange(instanceRange.getInstanceCount() * sizeof(DrawIndexedIndirectArgs));
				const BufferView countBuffView =
					BufferView(args.m_legacy.m_mdiDrawCountsBuffer).incrementOffset(sizeof(U32) * bucketIdx).setRange(sizeof(U32));
				cmdb.drawIndirectCount(state.m_primitiveTopology, indirectArgsBuffView, sizeof(DrawIndexedIndirectArgs), countBuffView, maxDrawCount);
			}
		});

#if ANKI_STATS_ENABLED
	if(pplineQuery.isCreated())
	{
		cmdb.endPipelineQuery(pplineQuery.get());
	}
#endif
}

} // end namespace anki
// Copyright (C) 2009-present, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Renderer/Sky.h>
#include <AnKi/Renderer/Renderer.h>
#include <AnKi/Util/Tracer.h>
#include <AnKi/Scene/Components/SkyboxComponent.h>
#include <AnKi/Scene/Components/LightComponent.h>

namespace anki {

Error Sky::init()
{
	const Error err = initInternal();
	if(err)
	{
		ANKI_R_LOGE("Failed to initialize sky passes");
	}

	return Error::kNone;
}

Error Sky::initInternal()
{
	ANKI_R_LOGV("Initializing sky passes");

	ANKI_CHECK(loadShaderProgram("ShaderBinaries/Sky.ankiprogbin", {}, m_prog, m_transmittanceLutGrProg, "SkyTransmittanceLut"));
	ANKI_CHECK(loadShaderProgram("ShaderBinaries/Sky.ankiprogbin", {}, m_prog, m_multipleScatteringLutGrProg, "SkyMultipleScatteringLut"));
	ANKI_CHECK(loadShaderProgram("ShaderBinaries/Sky.ankiprogbin", {}, m_prog, m_skyLutGrProg, "SkyLut"));
	ANKI_CHECK(loadShaderProgram("ShaderBinaries/Sky.ankiprogbin", {}, m_prog, m_computeSunColorGrProg, "ComputeSunColor"));

	const TextureUsageBit usage = TextureUsageBit::kAllCompute;
	const TextureUsageBit initialUsage = TextureUsageBit::kAllCompute;
	const Format formatB =
		(GrManager::getSingleton().getDeviceCapabilities().m_unalignedBbpTextureFormats) ? Format::kR16G16B16_Unorm : Format::kR16G16B16A16_Unorm;

	m_transmittanceLut = getRenderer().createAndClearRenderTarget(
		getRenderer().create2DRenderTargetInitInfo(kTransmittanceLutSize.x(), kTransmittanceLutSize.y(), formatB, usage, "SkyTransmittanceLut"),
		initialUsage);

	m_multipleScatteringLut = getRenderer().createAndClearRenderTarget(
		getRenderer().create2DRenderTargetInitInfo(kMultipleScatteringLutSize.x(), kMultipleScatteringLutSize.y(), formatB, usage,
												   "SkyMultipleScatteringLut"),
		initialUsage);

	m_skyLut = getRenderer().createAndClearRenderTarget(
		getRenderer().create2DRenderTargetInitInfo(kSkyLutSize.x(), kSkyLutSize.y(), formatB, usage | TextureUsageBit::kSampledFragment, "SkyLut"),
		initialUsage);

	return Error::kNone;
}

void Sky::populateRenderGraph(RenderingContext& ctx)
{
	ANKI_TRACE_SCOPED_EVENT(Sky);

	if(!isEnabled())
	{
		m_runCtx = {};
		return;
	}

	RenderGraphBuilder& rgraph = ctx.m_renderGraphDescr;

	const LightComponent* dirLightc = SceneGraph::getSingleton().getDirectionalLight();
	ANKI_ASSERT(dirLightc);
	const F32 sunPower = dirLightc->getLightPower();
	const Bool renderSkyLut = dirLightc->getDirection() != m_sunDir || m_sunPower != sunPower;
	const Bool renderTransAndMultiScatLuts = !m_transmittanceAndMultiScatterLutsGenerated;

	m_sunDir = dirLightc->getDirection();
	m_sunPower = sunPower;

	// Create render targets
	RenderTargetHandle transmittanceLutRt;
	RenderTargetHandle multipleScatteringLutRt;
	if(renderTransAndMultiScatLuts)
	{
		transmittanceLutRt = rgraph.importRenderTarget(m_transmittanceLut.get(), TextureUsageBit::kAllCompute);
		multipleScatteringLutRt = rgraph.importRenderTarget(m_multipleScatteringLut.get(), TextureUsageBit::kAllCompute);
		m_transmittanceAndMultiScatterLutsGenerated = true;
	}
	else
	{
		transmittanceLutRt = rgraph.importRenderTarget(m_transmittanceLut.get(), TextureUsageBit::kSampledCompute);
		multipleScatteringLutRt = rgraph.importRenderTarget(m_multipleScatteringLut.get(), TextureUsageBit::kSampledCompute);
	}

	if(m_skyLutImportedOnce) [[likely]]
	{
		m_runCtx.m_skyLutRt = rgraph.importRenderTarget(m_skyLut.get());
	}
	else
	{
		m_runCtx.m_skyLutRt = rgraph.importRenderTarget(m_skyLut.get(), TextureUsageBit::kAllCompute);
		m_skyLutImportedOnce = true;
	}

	// Transmittance LUT
	if(renderTransAndMultiScatLuts)
	{
		NonGraphicsRenderPass& rpass = rgraph.newNonGraphicsRenderPass("SkyTransmittanceLut");

		rpass.newTextureDependency(transmittanceLutRt, TextureUsageBit::kStorageComputeWrite);

		rpass.setWork([this, transmittanceLutRt](RenderPassWorkContext& rgraphCtx) {
			ANKI_TRACE_SCOPED_EVENT(SkyTransmittanceLut);

			CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

			cmdb.bindShaderProgram(m_transmittanceLutGrProg.get());

			rgraphCtx.bindTexture(ANKI_REG(u0), transmittanceLutRt);

			dispatchPPCompute(cmdb, 8, 8, kTransmittanceLutSize.x(), kTransmittanceLutSize.y());
		});
	}

	// Multiple scattering LUT
	if(renderTransAndMultiScatLuts)
	{
		NonGraphicsRenderPass& rpass = rgraph.newNonGraphicsRenderPass("SkyMultipleScatteringLut");

		rpass.newTextureDependency(transmittanceLutRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(multipleScatteringLutRt, TextureUsageBit::kStorageComputeWrite);

		rpass.setWork([this, transmittanceLutRt, multipleScatteringLutRt](RenderPassWorkContext& rgraphCtx) {
			ANKI_TRACE_SCOPED_EVENT(SkyMultipleScatteringLut);

			CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

			cmdb.bindShaderProgram(m_multipleScatteringLutGrProg.get());

			rgraphCtx.bindTexture(ANKI_REG(t0), transmittanceLutRt);
			cmdb.bindSampler(ANKI_REG(s0), getRenderer().getSamplers().m_trilinearClamp.get());
			rgraphCtx.bindTexture(ANKI_REG(u0), multipleScatteringLutRt);

			dispatchPPCompute(cmdb, 8, 8, kMultipleScatteringLutSize.x(), kMultipleScatteringLutSize.y());
		});
	}

	// Sky LUT
	if(renderSkyLut)
	{
		NonGraphicsRenderPass& rpass = rgraph.newNonGraphicsRenderPass("SkyLut");

		rpass.newTextureDependency(transmittanceLutRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(multipleScatteringLutRt, TextureUsageBit::kSampledCompute);
		rpass.newTextureDependency(m_runCtx.m_skyLutRt, TextureUsageBit::kStorageComputeWrite);

		rpass.setWork([this, transmittanceLutRt, multipleScatteringLutRt, &ctx](RenderPassWorkContext& rgraphCtx) {
			ANKI_TRACE_SCOPED_EVENT(SkyLut);

			CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

			cmdb.bindShaderProgram(m_skyLutGrProg.get());

			rgraphCtx.bindTexture(ANKI_REG(t0), transmittanceLutRt);
			rgraphCtx.bindTexture(ANKI_REG(t1), multipleScatteringLutRt);
			cmdb.bindSampler(ANKI_REG(s0), getRenderer().getSamplers().m_trilinearClamp.get());
			rgraphCtx.bindTexture(ANKI_REG(u0), m_runCtx.m_skyLutRt);
			cmdb.bindUniformBuffer(ANKI_REG(b0), ctx.m_globalRenderingUniformsBuffer);

			dispatchPPCompute(cmdb, 8, 8, kSkyLutSize.x(), kSkyLutSize.y());
		});
	}

	// Compute sun color always
	{
		NonGraphicsRenderPass& rpass = rgraph.newNonGraphicsRenderPass("ComputeSunColor");

		rpass.newTextureDependency(transmittanceLutRt, TextureUsageBit::kSampledCompute);

		rpass.setWork([this, transmittanceLutRt, &ctx](RenderPassWorkContext& rgraphCtx) {
			ANKI_TRACE_SCOPED_EVENT(ComputeSunColor);

			CommandBuffer& cmdb = *rgraphCtx.m_commandBuffer;

			cmdb.bindShaderProgram(m_computeSunColorGrProg.get());

			rgraphCtx.bindTexture(ANKI_REG(t0), transmittanceLutRt);
			cmdb.bindStorageBuffer(ANKI_REG(u0), ctx.m_globalRenderingUniformsBuffer);

			cmdb.dispatchCompute(1, 1, 1);
		});
	}
}

Bool Sky::isEnabled() const
{
	const SkyboxComponent* skyc = SceneGraph::getSingleton().getSkybox();
	const LightComponent* dirLightc = SceneGraph::getSingleton().getDirectionalLight();

	return skyc && skyc->getSkyboxType() == SkyboxType::kGenerated && dirLightc;
}

} // end namespace anki
set(RP_COMMON_DIR ${ZETA_RENDER_PASS_DIR}/Common)
set(RP_COMMON_SRC
    ${RP_COMMON_DIR}/BRDF.hlsli
    ${RP_COMMON_DIR}/Math.hlsli
    ${RP_COMMON_DIR}/Common.hlsli
    ${RP_COMMON_DIR}/FrameConstants.h
    ${RP_COMMON_DIR}/GBuffers.hlsli
    ${RP_COMMON_DIR}/RT.hlsli
    ${RP_COMMON_DIR}/Sampling.hlsli
    ${RP_COMMON_DIR}/SH.hlsli
    ${RP_COMMON_DIR}/StaticTextureSamplers.hlsli
    ${RP_COMMON_DIR}/VolumetricLighting.hlsli
	${RP_COMMON_DIR}/ffx_spd.h
    ${RP_COMMON_DIR}/ffx_a.h)
set(RP_COMMON_SRC ${RP_COMMON_SRC} PARENT_SCOPE)
/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gl/GrGLShaderBuilder.h"
#include "gl/GrGLProgram.h"
#include "gl/GrGLUniformHandle.h"
#include "GrCoordTransform.h"
#include "GrDrawEffect.h"
#include "GrGpuGL.h"
#include "GrTexture.h"
#include "SkRTConf.h"
#include "SkTrace.h"

#define GL_CALL(X) GR_GL_CALL(fGpu->glInterface(), X)
#define GL_CALL_RET(R, X) GR_GL_CALL_RET(fGpu->glInterface(), R, X)

// number of each input/output type in a single allocation block
static const int kVarsPerBlock = 8;

// except FS outputs where we expect 2 at most.
static const int kMaxFSOutputs = 2;

// ES2 FS only guarantees mediump and lowp support
static const GrGLShaderVar::Precision kDefaultFragmentPrecision = GrGLShaderVar::kMedium_Precision;

typedef GrGLUniformManager::UniformHandle UniformHandle;

SK_CONF_DECLARE(bool, c_PrintShaders, "gpu.printShaders", false,
                "Print the source code for all shaders generated.");

///////////////////////////////////////////////////////////////////////////////

namespace {

inline const char* color_attribute_name() { return "aColor"; }
inline const char* coverage_attribute_name() { return "aCoverage"; }
inline const char* declared_color_output_name() { return "fsColorOut"; }
inline const char* dual_source_output_name() { return "dualSourceOut"; }
inline const char* sample_function_name(GrSLType type, GrGLSLGeneration glslGen) {
    if (kVec2f_GrSLType == type) {
        return glslGen >= k130_GrGLSLGeneration ? "texture" : "texture2D";
    } else {
        SkASSERT(kVec3f_GrSLType == type);
        return glslGen >= k130_GrGLSLGeneration ? "textureProj" : "texture2DProj";
    }
}

/**
 * Do we need to either map r,g,b->a or a->r. configComponentMask indicates which channels are
 * present in the texture's config. swizzleComponentMask indicates the channels present in the
 * shader swizzle.
 */
inline bool swizzle_requires_alpha_remapping(const GrGLCaps& caps,
                                             uint32_t configComponentMask,
                                             uint32_t swizzleComponentMask) {
    if (caps.textureSwizzleSupport()) {
        // Any remapping is handled using texture swizzling not shader modifications.
        return false;
    }
    // check if the texture is alpha-only
    if (kA_GrColorComponentFlag == configComponentMask) {
        if (caps.textureRedSupport() && (kA_GrColorComponentFlag & swizzleComponentMask)) {
            // we must map the swizzle 'a's to 'r'.
            return true;
        }
        if (kRGB_GrColorComponentFlags & swizzleComponentMask) {
            // The 'r', 'g', and/or 'b's must be mapped to 'a' according to our semantics that
            // alpha-only textures smear alpha across all four channels when read.
            return true;
        }
    }
    return false;
}

void append_swizzle(SkString* outAppend,
                    const GrGLShaderBuilder::TextureSampler& texSampler,
                    const GrGLCaps& caps) {
    const char* swizzle = texSampler.swizzle();
    char mangledSwizzle[5];

    // The swizzling occurs using texture params instead of shader-mangling if ARB_texture_swizzle
    // is available.
    if (!caps.textureSwizzleSupport() &&
        (kA_GrColorComponentFlag == texSampler.configComponentMask())) {
        char alphaChar = caps.textureRedSupport() ? 'r' : 'a';
        int i;
        for (i = 0; '\0' != swizzle[i]; ++i) {
            mangledSwizzle[i] = alphaChar;
        }
        mangledSwizzle[i] ='\0';
        swizzle = mangledSwizzle;
    }
    // For shader prettiness we omit the swizzle rather than appending ".rgba".
    if (memcmp(swizzle, "rgba", 4)) {
        outAppend->appendf(".%s", swizzle);
    }
}

}

static const char kDstCopyColorName[] = "_dstColor";

///////////////////////////////////////////////////////////////////////////////

GrGLShaderBuilder::GrGLShaderBuilder(GrGpuGL* gpu,
                                     GrGLUniformManager& uniformManager,
                                     const GrGLProgramDesc& desc,
                                     bool needsVertexShader)
    : fUniforms(kVarsPerBlock)
    , fGpu(gpu)
    , fUniformManager(uniformManager)
    , fFSFeaturesAddedMask(0)
    , fFSInputs(kVarsPerBlock)
    , fFSOutputs(kMaxFSOutputs)
    , fSetupFragPosition(false)
    , fKnownColorValue(kNone_GrSLConstantVec)
    , fKnownCoverageValue(kNone_GrSLConstantVec)
    , fHasCustomColorOutput(false)
    , fHasSecondaryOutput(false)
    , fTopLeftFragPosRead(kTopLeftFragPosRead_FragPosKey == desc.getHeader().fFragPosKey) {

    const GrGLProgramDesc::KeyHeader& header = desc.getHeader();

    if (needsVertexShader) {
        fVertexBuilder.reset(SkNEW_ARGS(VertexBuilder, (this, fGpu, desc)));
    }

    // Emit code to read the dst copy textue if necessary.
    if (kNoDstRead_DstReadKey != header.fDstReadKey &&
        GrGLCaps::kNone_FBFetchType == fGpu->glCaps().fbFetchType()) {
        bool topDown = SkToBool(kTopLeftOrigin_DstReadKeyBit & header.fDstReadKey);
        const char* dstCopyTopLeftName;
        const char* dstCopyCoordScaleName;
        uint32_t configMask;
        if (SkToBool(kUseAlphaConfig_DstReadKeyBit & header.fDstReadKey)) {
            configMask = kA_GrColorComponentFlag;
        } else {
            configMask = kRGBA_GrColorComponentFlags;
        }
        fDstCopySampler.init(this, configMask, "rgba", 0);

        fDstCopyTopLeftUniform = this->addUniform(kFragment_Visibility,
                                                  kVec2f_GrSLType,
                                                  "DstCopyUpperLeft",
                                                  &dstCopyTopLeftName);
        fDstCopyScaleUniform     = this->addUniform(kFragment_Visibility,
                                                    kVec2f_GrSLType,
                                                    "DstCopyCoordScale",
                                                    &dstCopyCoordScaleName);
        const char* fragPos = this->fragmentPosition();
        this->fsCodeAppend("\t// Read color from copy of the destination.\n");
        this->fsCodeAppendf("\tvec2 _dstTexCoord = (%s.xy - %s) * %s;\n",
                            fragPos, dstCopyTopLeftName, dstCopyCoordScaleName);
        if (!topDown) {
            this->fsCodeAppend("\t_dstTexCoord.y = 1.0 - _dstTexCoord.y;\n");
        }
        this->fsCodeAppendf("\tvec4 %s = ", kDstCopyColorName);
        this->fsAppendTextureLookup(fDstCopySampler, "_dstTexCoord");
        this->fsCodeAppend(";\n\n");
    }

    switch (header.fColorInput) {
        case GrGLProgramDesc::kAttribute_ColorInput: {
            SkASSERT(NULL != fVertexBuilder.get());
            fVertexBuilder->addAttribute(kVec4f_GrSLType, color_attribute_name());
            const char *vsName, *fsName;
            fVertexBuilder->addVarying(kVec4f_GrSLType, "Color", &vsName, &fsName);
            fVertexBuilder->vsCodeAppendf("\t%s = %s;\n", vsName, color_attribute_name());
            fInputColor = fsName;
            break;
        }
        case GrGLProgramDesc::kUniform_ColorInput: {
            const char* name;
            fColorUniform = this->addUniform(GrGLShaderBuilder::kFragment_Visibility,
                                             kVec4f_GrSLType, "Color", &name);
            fInputColor = name;
            break;
        }
        case GrGLProgramDesc::kTransBlack_ColorInput:
            fKnownColorValue = kZeros_GrSLConstantVec;
            break;
        case GrGLProgramDesc::kSolidWhite_ColorInput:
            fKnownColorValue = kOnes_GrSLConstantVec;
            break;
        default:
            GrCrash("Unknown color type.");
    }

    switch (header.fCoverageInput) {
        case GrGLProgramDesc::kAttribute_ColorInput: {
            SkASSERT(NULL != fVertexBuilder.get());
            fVertexBuilder->addAttribute(kVec4f_GrSLType, coverage_attribute_name());
            const char *vsName, *fsName;
            fVertexBuilder->addVarying(kVec4f_GrSLType, "Coverage", &vsName, &fsName);
            fVertexBuilder->vsCodeAppendf("\t%s = %s;\n", vsName, coverage_attribute_name());
            fInputCoverage = fsName;
            break;
        }
        case GrGLProgramDesc::kUniform_ColorInput: {
            const char* name;
            fCoverageUniform = this->addUniform(GrGLShaderBuilder::kFragment_Visibility,
                                                kVec4f_GrSLType, "Coverage", &name);
            fInputCoverage = name;
            break;
        }
        case GrGLProgramDesc::kTransBlack_ColorInput:
            fKnownCoverageValue = kZeros_GrSLConstantVec;
            break;
        case GrGLProgramDesc::kSolidWhite_ColorInput:
            fKnownCoverageValue = kOnes_GrSLConstantVec;
            break;
        default:
            GrCrash("Unknown coverage type.");
    }

    if (k110_GrGLSLGeneration != fGpu->glslGeneration()) {
        fFSOutputs.push_back().set(kVec4f_GrSLType,
                                   GrGLShaderVar::kOut_TypeModifier,
                                   declared_color_output_name());
        fHasCustomColorOutput = true;
    }
}

bool GrGLShaderBuilder::enableFeature(GLSLFeature feature) {
    switch (feature) {
        case kStandardDerivatives_GLSLFeature:
            if (!fGpu->glCaps().shaderDerivativeSupport()) {
                return false;
            }
            if (kES_GrGLBinding == fGpu->glBinding()) {
                this->addFSFeature(1 << kStandardDerivatives_GLSLFeature,
                                   "GL_OES_standard_derivatives");
            }
            return true;
        default:
            GrCrash("Unexpected GLSLFeature requested.");
            return false;
    }
}

bool GrGLShaderBuilder::enablePrivateFeature(GLSLPrivateFeature feature) {
    switch (feature) {
        case kFragCoordConventions_GLSLPrivateFeature:
            if (!fGpu->glCaps().fragCoordConventionsSupport()) {
                return false;
            }
            if (fGpu->glslGeneration() < k150_GrGLSLGeneration) {
                this->addFSFeature(1 << kFragCoordConventions_GLSLPrivateFeature,
                                   "GL_ARB_fragment_coord_conventions");
            }
            return true;
        case kEXTShaderFramebufferFetch_GLSLPrivateFeature:
            if (GrGLCaps::kEXT_FBFetchType != fGpu->glCaps().fbFetchType()) {
                return false;
            }
            this->addFSFeature(1 << kEXTShaderFramebufferFetch_GLSLPrivateFeature,
                               "GL_EXT_shader_framebuffer_fetch");
            return true;
        case kNVShaderFramebufferFetch_GLSLPrivateFeature:
            if (GrGLCaps::kNV_FBFetchType != fGpu->glCaps().fbFetchType()) {
                return false;
            }
            this->addFSFeature(1 << kNVShaderFramebufferFetch_GLSLPrivateFeature,
                               "GL_NV_shader_framebuffer_fetch");
            return true;
        default:
            GrCrash("Unexpected GLSLPrivateFeature requested.");
            return false;
    }
}

void GrGLShaderBuilder::addFSFeature(uint32_t featureBit, const char* extensionName) {
    if (!(featureBit & fFSFeaturesAddedMask)) {
        fFSExtensions.appendf("#extension %s: require\n", extensionName);
        fFSFeaturesAddedMask |= featureBit;
    }
}

void GrGLShaderBuilder::nameVariable(SkString* out, char prefix, const char* name) {
    if ('\0' == prefix) {
        *out = name;
    } else {
        out->printf("%c%s", prefix, name);
    }
    if (fCodeStage.inStageCode()) {
        if (out->endsWith('_')) {
            // Names containing "__" are reserved.
            out->append("x");
        }
        out->appendf("_Stage%d", fCodeStage.stageIndex());
    }
}

const char* GrGLShaderBuilder::dstColor() {
    if (fCodeStage.inStageCode()) {
        const GrEffectRef& effect = *fCodeStage.effectStage()->getEffect();
        if (!effect->willReadDstColor()) {
            GrDebugCrash("GrGLEffect asked for dst color but its generating GrEffect "
                         "did not request access.");
            return "";
        }
    }
    static const char kFBFetchColorName[] = "gl_LastFragData[0]";
    GrGLCaps::FBFetchType fetchType = fGpu->glCaps().fbFetchType();
    if (GrGLCaps::kEXT_FBFetchType == fetchType) {
        SkAssertResult(this->enablePrivateFeature(kEXTShaderFramebufferFetch_GLSLPrivateFeature));
        return kFBFetchColorName;
    } else if (GrGLCaps::kNV_FBFetchType == fetchType) {
        SkAssertResult(this->enablePrivateFeature(kNVShaderFramebufferFetch_GLSLPrivateFeature));
        return kFBFetchColorName;
    } else if (fDstCopySampler.isInitialized()) {
        return kDstCopyColorName;
    } else {
        return "";
    }
}

void GrGLShaderBuilder::appendTextureLookup(SkString* out,
                                            const GrGLShaderBuilder::TextureSampler& sampler,
                                            const char* coordName,
                                            GrSLType varyingType) const {
    SkASSERT(NULL != coordName);

    out->appendf("%s(%s, %s)",
                 sample_function_name(varyingType, fGpu->glslGeneration()),
                 this->getUniformCStr(sampler.fSamplerUniform),
                 coordName);
    append_swizzle(out, sampler, fGpu->glCaps());
}

void GrGLShaderBuilder::fsAppendTextureLookup(const GrGLShaderBuilder::TextureSampler& sampler,
                                              const char* coordName,
                                              GrSLType varyingType) {
    this->appendTextureLookup(&fFSCode, sampler, coordName, varyingType);
}

void GrGLShaderBuilder::fsAppendTextureLookupAndModulate(
                                            const char* modulation,
                                            const GrGLShaderBuilder::TextureSampler& sampler,
                                            const char* coordName,
                                            GrSLType varyingType) {
    SkString lookup;
    this->appendTextureLookup(&lookup, sampler, coordName, varyingType);
    GrGLSLModulatef<4>(&fFSCode, modulation, lookup.c_str());
}

GrGLShaderBuilder::EffectKey GrGLShaderBuilder::KeyForTextureAccess(const GrTextureAccess& access,
                                                                    const GrGLCaps& caps) {
    uint32_t configComponentMask = GrPixelConfigComponentMask(access.getTexture()->config());
    if (swizzle_requires_alpha_remapping(caps, configComponentMask, access.swizzleMask())) {
        return 1;
    } else {
        return 0;
    }
}

GrGLShaderBuilder::DstReadKey GrGLShaderBuilder::KeyForDstRead(const GrTexture* dstCopy,
                                                               const GrGLCaps& caps) {
    uint32_t key = kYesDstRead_DstReadKeyBit;
    if (GrGLCaps::kNone_FBFetchType != caps.fbFetchType()) {
        return key;
    }
    SkASSERT(NULL != dstCopy);
    if (!caps.textureSwizzleSupport() && GrPixelConfigIsAlphaOnly(dstCopy->config())) {
        // The fact that the config is alpha-only must be considered when generating code.
        key |= kUseAlphaConfig_DstReadKeyBit;
    }
    if (kTopLeft_GrSurfaceOrigin == dstCopy->origin()) {
        key |= kTopLeftOrigin_DstReadKeyBit;
    }
    SkASSERT(static_cast<DstReadKey>(key) == key);
    return static_cast<DstReadKey>(key);
}

GrGLShaderBuilder::FragPosKey GrGLShaderBuilder::KeyForFragmentPosition(const GrRenderTarget* dst,
                                                                        const GrGLCaps&) {
    if (kTopLeft_GrSurfaceOrigin == dst->origin()) {
        return kTopLeftFragPosRead_FragPosKey;
    } else {
        return kBottomLeftFragPosRead_FragPosKey;
    }
}


const GrGLenum* GrGLShaderBuilder::GetTexParamSwizzle(GrPixelConfig config, const GrGLCaps& caps) {
    if (caps.textureSwizzleSupport() && GrPixelConfigIsAlphaOnly(config)) {
        if (caps.textureRedSupport()) {
            static const GrGLenum gRedSmear[] = { GR_GL_RED, GR_GL_RED, GR_GL_RED, GR_GL_RED };
            return gRedSmear;
        } else {
            static const GrGLenum gAlphaSmear[] = { GR_GL_ALPHA, GR_GL_ALPHA,
                                                    GR_GL_ALPHA, GR_GL_ALPHA };
            return gAlphaSmear;
        }
    } else {
        static const GrGLenum gStraight[] = { GR_GL_RED, GR_GL_GREEN, GR_GL_BLUE, GR_GL_ALPHA };
        return gStraight;
    }
}

GrGLUniformManager::UniformHandle GrGLShaderBuilder::addUniformArray(uint32_t visibility,
                                                                     GrSLType type,
                                                                     const char* name,
                                                                     int count,
                                                                     const char** outName) {
    SkASSERT(name && strlen(name));
    SkDEBUGCODE(static const uint32_t kVisibilityMask = kVertex_Visibility | kFragment_Visibility);
    SkASSERT(0 == (~kVisibilityMask & visibility));
    SkASSERT(0 != visibility);

    BuilderUniform& uni = fUniforms.push_back();
    UniformHandle h = GrGLUniformManager::UniformHandle::CreateFromUniformIndex(fUniforms.count() - 1);
    SkDEBUGCODE(UniformHandle h2 =)
    fUniformManager.appendUniform(type, count);
    // We expect the uniform manager to initially have no uniforms and that all uniforms are added
    // by this function. Therefore, the handles should match.
    SkASSERT(h2 == h);
    uni.fVariable.setType(type);
    uni.fVariable.setTypeModifier(GrGLShaderVar::kUniform_TypeModifier);
    this->nameVariable(uni.fVariable.accessName(), 'u', name);
    uni.fVariable.setArrayCount(count);
    uni.fVisibility = visibility;

    // If it is visible in both the VS and FS, the precision must match.
    // We declare a default FS precision, but not a default VS. So set the var
    // to use the default FS precision.
    if ((kVertex_Visibility | kFragment_Visibility) == visibility) {
        // the fragment and vertex precisions must match
        uni.fVariable.setPrecision(kDefaultFragmentPrecision);
    }

    if (NULL != outName) {
        *outName = uni.fVariable.c_str();
    }

    return h;
}

SkString GrGLShaderBuilder::ensureFSCoords2D(const TransformedCoordsArray& coords, int index) {
    if (kVec3f_GrSLType != coords[index].type()) {
        SkASSERT(kVec2f_GrSLType == coords[index].type());
        return coords[index].getName();
    }

    SkString coords2D("coords2D");
    if (0 != index) {
        coords2D.appendf("_%i", index);
    }
    this->fsCodeAppendf("\tvec2 %s = %s.xy / %s.z;",
                        coords2D.c_str(), coords[index].c_str(), coords[index].c_str());
    return coords2D;
}

const char* GrGLShaderBuilder::fragmentPosition() {
    if (fCodeStage.inStageCode()) {
        const GrEffectRef& effect = *fCodeStage.effectStage()->getEffect();
        if (!effect->willReadFragmentPosition()) {
            GrDebugCrash("GrGLEffect asked for frag position but its generating GrEffect "
                         "did not request access.");
            return "";
        }
    }
    if (fTopLeftFragPosRead) {
        if (!fSetupFragPosition) {
            fFSInputs.push_back().set(kVec4f_GrSLType,
                                      GrGLShaderVar::kIn_TypeModifier,
                                      "gl_FragCoord",
                                      GrGLShaderVar::kDefault_Precision);
            fSetupFragPosition = true;
        }
        return "gl_FragCoord";
    } else if (fGpu->glCaps().fragCoordConventionsSupport()) {
        if (!fSetupFragPosition) {
            SkAssertResult(this->enablePrivateFeature(kFragCoordConventions_GLSLPrivateFeature));
            fFSInputs.push_back().set(kVec4f_GrSLType,
                                      GrGLShaderVar::kIn_TypeModifier,
                                      "gl_FragCoord",
                                      GrGLShaderVar::kDefault_Precision,
                                      GrGLShaderVar::kUpperLeft_Origin);
            fSetupFragPosition = true;
        }
        return "gl_FragCoord";
    } else {
        static const char* kCoordName = "fragCoordYDown";
        if (!fSetupFragPosition) {
            // temporarily change the stage index because we're inserting non-stage code.
            CodeStage::AutoStageRestore csar(&fCodeStage, NULL);

            SkASSERT(!fRTHeightUniform.isValid());
            const char* rtHeightName;

            fRTHeightUniform = this->addUniform(kFragment_Visibility,
                                                kFloat_GrSLType,
                                                "RTHeight",
                                                &rtHeightName);

            this->fFSCode.prependf("\tvec4 %s = vec4(gl_FragCoord.x, %s - gl_FragCoord.y, gl_FragCoord.zw);\n",
                                   kCoordName, rtHeightName);
            fSetupFragPosition = true;
        }
        SkASSERT(fRTHeightUniform.isValid());
        return kCoordName;
    }
}


void GrGLShaderBuilder::fsEmitFunction(GrSLType returnType,
                                       const char* name,
                                       int argCnt,
                                       const GrGLShaderVar* args,
                                       const char* body,
                                       SkString* outName) {
    fFSFunctions.append(GrGLSLTypeString(returnType));
    this->nameVariable(outName, '\0', name);
    fFSFunctions.appendf(" %s", outName->c_str());
    fFSFunctions.append("(");
    for (int i = 0; i < argCnt; ++i) {
        args[i].appendDecl(this->ctxInfo(), &fFSFunctions);
        if (i < argCnt - 1) {
            fFSFunctions.append(", ");
        }
    }
    fFSFunctions.append(") {\n");
    fFSFunctions.append(body);
    fFSFunctions.append("}\n\n");
}

namespace {

inline void append_default_precision_qualifier(GrGLShaderVar::Precision p,
                                               GrGLBinding binding,
                                               SkString* str) {
    // Desktop GLSL has added precision qualifiers but they don't do anything.
    if (kES_GrGLBinding == binding) {
        switch (p) {
            case GrGLShaderVar::kHigh_Precision:
                str->append("precision highp float;\n");
                break;
            case GrGLShaderVar::kMedium_Precision:
                str->append("precision mediump float;\n");
                break;
            case GrGLShaderVar::kLow_Precision:
                str->append("precision lowp float;\n");
                break;
            case GrGLShaderVar::kDefault_Precision:
                GrCrash("Default precision now allowed.");
            default:
                GrCrash("Unknown precision value.");
        }
    }
}
}

void GrGLShaderBuilder::appendDecls(const VarArray& vars, SkString* out) const {
    for (int i = 0; i < vars.count(); ++i) {
        vars[i].appendDecl(this->ctxInfo(), out);
        out->append(";\n");
    }
}

void GrGLShaderBuilder::appendUniformDecls(ShaderVisibility visibility,
                                           SkString* out) const {
    for (int i = 0; i < fUniforms.count(); ++i) {
        if (fUniforms[i].fVisibility & visibility) {
            fUniforms[i].fVariable.appendDecl(this->ctxInfo(), out);
            out->append(";\n");
        }
    }
}

void GrGLShaderBuilder::emitEffects(
                        const GrEffectStage* effectStages[],
                        const EffectKey effectKeys[],
                        int effectCnt,
                        SkString* fsInOutColor,
                        GrSLConstantVec* fsInOutColorKnownValue,
                        SkTArray<GrGLCoordTransform, false>* effectCoordTransformArrays[],
                        SkTArray<GrGLUniformManager::UniformHandle, true>* effectSamplerHandles[],
                        GrGLEffect* glEffects[]) {
    bool effectEmitted = false;

    SkString inColor = *fsInOutColor;
    SkString outColor;

    for (int e = 0; e < effectCnt; ++e) {
        SkASSERT(NULL != effectStages[e] && NULL != effectStages[e]->getEffect());
        const GrEffectStage& stage = *effectStages[e];
        const GrEffectRef& effect = *stage.getEffect();

        CodeStage::AutoStageRestore csar(&fCodeStage, &stage);

        int numTransforms = effect->numTransforms();
        SkSTArray<8, GrGLCoordTransform::TransformedCoords> transformedCoords;
        transformedCoords.push_back_n(numTransforms);
        EffectKey transformKey = GrBackendEffectFactory::GetTransformKey(effectKeys[e]);
        for (int c = 0; c < numTransforms; ++c) {
            GrGLCoordTransform& ct = effectCoordTransformArrays[e]->push_back();
            EffectKey key = (transformKey >> (c * GrGLCoordTransform::kKeyBits)) &
                            (GrGLCoordTransform::kKeyMask);
            ct.emitCode(this, key, &transformedCoords[c], c);
        }

        int numTextures = effect->numTextures();
        SkSTArray<8, GrGLShaderBuilder::TextureSampler> textureSamplers;
        textureSamplers.push_back_n(numTextures);
        for (int t = 0; t < numTextures; ++t) {
            textureSamplers[t].init(this, &effect->textureAccess(t), t);
            effectSamplerHandles[e]->push_back(textureSamplers[t].fSamplerUniform);
        }

        GrDrawEffect drawEffect(stage, NULL != fVertexBuilder.get()
                                       && fVertexBuilder->hasExplicitLocalCoords());

        int numAttributes = stage.getVertexAttribIndexCount();
        const int* attributeIndices = stage.getVertexAttribIndices();
        SkSTArray<GrEffect::kMaxVertexAttribs, SkString> attributeNames;
        for (int a = 0; a < numAttributes; ++a) {
            // TODO: Make addAttribute mangle the name.
            SkASSERT(NULL != fVertexBuilder.get());
            SkString attributeName("aAttr");
            attributeName.appendS32(attributeIndices[a]);
            fVertexBuilder->addEffectAttribute(attributeIndices[a],
                                               effect->vertexAttribType(a),
                                               attributeName);
        }

        glEffects[e] = effect->getFactory().createGLInstance(drawEffect);

        if (kZeros_GrSLConstantVec == *fsInOutColorKnownValue) {
            // Effects have no way to communicate zeros, they treat an empty string as ones.
            this->nameVariable(&inColor, '\0', "input");
            this->fsCodeAppendf("\tvec4 %s = %s;\n", inColor.c_str(), GrGLSLZerosVecf(4));
        }

        // create var to hold stage result
        this->nameVariable(&outColor, '\0', "output");
        this->fsCodeAppendf("\tvec4 %s;\n", outColor.c_str());

        // Enclose custom code in a block to avoid namespace conflicts
        SkString openBrace;
        openBrace.printf("\t{ // Stage %d: %s\n", fCodeStage.stageIndex(), glEffects[e]->name());
        if (NULL != fVertexBuilder.get()) {
            fVertexBuilder->vsCodeAppend(openBrace.c_str());
        }
        this->fsCodeAppend(openBrace.c_str());

        glEffects[e]->emitCode(this,
                               drawEffect,
                               effectKeys[e],
                               outColor.c_str(),
                               inColor.isEmpty() ? NULL : inColor.c_str(),
                               transformedCoords,
                               textureSamplers);

        if (NULL != fVertexBuilder.get()) {
            fVertexBuilder->vsCodeAppend("\t}\n");
        }
        this->fsCodeAppend("\t}\n");

        inColor = outColor;
        *fsInOutColorKnownValue = kNone_GrSLConstantVec;
        effectEmitted = true;
    }

    if (effectEmitted) {
        *fsInOutColor = outColor;
    }
}

const char* GrGLShaderBuilder::getColorOutputName() const {
    return fHasCustomColorOutput ? declared_color_output_name() : "gl_FragColor";
}

const char* GrGLShaderBuilder::enableSecondaryOutput() {
    if (!fHasSecondaryOutput) {
        fFSOutputs.push_back().set(kVec4f_GrSLType,
                                   GrGLShaderVar::kOut_TypeModifier,
                                   dual_source_output_name());
        fHasSecondaryOutput = true;
    }
    return dual_source_output_name();
}


bool GrGLShaderBuilder::finish(GrGLuint* outProgramId) {
    SK_TRACE_EVENT0("GrGLShaderBuilder::finish");

    GrGLuint programId = 0;
    GL_CALL_RET(programId, CreateProgram());
    if (!programId) {
        return false;
    }

    if (!this->compileAndAttachShaders(programId)) {
        GL_CALL(DeleteProgram(programId));
        return false;
    }

    this->bindProgramLocations(programId);

    GL_CALL(LinkProgram(programId));
    GrGLint linked = GR_GL_INIT_ZERO;
    GL_CALL(GetProgramiv(programId, GR_GL_LINK_STATUS, &linked));
    if (!linked) {
        GrGLint infoLen = GR_GL_INIT_ZERO;
        GL_CALL(GetProgramiv(programId, GR_GL_INFO_LOG_LENGTH, &infoLen));
        SkAutoMalloc log(sizeof(char)*(infoLen+1));  // outside if for debugger
        if (infoLen > 0) {
            // retrieve length even though we don't need it to workaround
            // bug in chrome cmd buffer param validation.
            GrGLsizei length = GR_GL_INIT_ZERO;
            GL_CALL(GetProgramInfoLog(programId,
                                      infoLen+1,
                                      &length,
                                      (char*)log.get()));
            GrPrintf((char*)log.get());
        }
        SkDEBUGFAIL("Error linking program");
        GL_CALL(DeleteProgram(programId));
        return false;
    }

    fUniformManager.getUniformLocations(programId, fUniforms);
    *outProgramId = programId;
    return true;
}

namespace {
// Compiles a GL shader, attaches it to a program, and releases the shader's reference.
// (That way there's no need to hang on to the GL shader id and delete it later.)
bool attach_shader(const GrGLInterface* gli,
                   GrGLuint programId,
                   GrGLenum type,
                   const SkString& shaderSrc) {
    GrGLuint shaderId;
    GR_GL_CALL_RET(gli, shaderId, CreateShader(type));
    if (0 == shaderId) {
        return false;
    }

    const GrGLchar* sourceStr = shaderSrc.c_str();
    int sourceLength = shaderSrc.size();
    GR_GL_CALL(gli, ShaderSource(shaderId, 1, &sourceStr, &sourceLength));

    GrGLint compiled = GR_GL_INIT_ZERO;
    GR_GL_CALL(gli, CompileShader(shaderId));
    GR_GL_CALL(gli, GetShaderiv(shaderId, GR_GL_COMPILE_STATUS, &compiled));

    if (!compiled) {
        GrGLint infoLen = GR_GL_INIT_ZERO;
        GR_GL_CALL(gli, GetShaderiv(shaderId, GR_GL_INFO_LOG_LENGTH, &infoLen));
        SkAutoMalloc log(sizeof(char)*(infoLen+1)); // outside if for debugger
        if (infoLen > 0) {
            // retrieve length even though we don't need it to workaround bug in chrome cmd buffer
            // param validation.
            GrGLsizei length = GR_GL_INIT_ZERO;
            GR_GL_CALL(gli, GetShaderInfoLog(shaderId, infoLen+1,
                                             &length, (char*)log.get()));
            GrPrintf(shaderSrc.c_str());
            GrPrintf("\n%s", log.get());
        }
        SkDEBUGFAIL("Shader compilation failed!");
        GR_GL_CALL(gli, DeleteShader(shaderId));
        return false;
    } else if (c_PrintShaders) {
        GrPrintf(shaderSrc.c_str());
        GrPrintf("\n");
    }

    GR_GL_CALL(gli, AttachShader(programId, shaderId));
    GR_GL_CALL(gli, DeleteShader(shaderId));
    return true;
}

}

bool GrGLShaderBuilder::compileAndAttachShaders(GrGLuint programId) const {
    if (NULL != fVertexBuilder.get() && !fVertexBuilder->compileAndAttachShaders(programId)) {
        return false;
    }

    SkString fragShaderSrc(GrGetGLSLVersionDecl(this->ctxInfo()));
    fragShaderSrc.append(fFSExtensions);
    append_default_precision_qualifier(kDefaultFragmentPrecision,
                                       fGpu->glBinding(),
                                       &fragShaderSrc);
    this->appendUniformDecls(kFragment_Visibility, &fragShaderSrc);
    this->appendDecls(fFSInputs, &fragShaderSrc);
    // We shouldn't have declared outputs on 1.10
    SkASSERT(k110_GrGLSLGeneration != fGpu->glslGeneration() || fFSOutputs.empty());
    this->appendDecls(fFSOutputs, &fragShaderSrc);
    fragShaderSrc.append(fFSFunctions);
    fragShaderSrc.append("void main() {\n");
    fragShaderSrc.append(fFSCode);
    fragShaderSrc.append("}\n");
    if (!attach_shader(fGpu->glInterface(), programId, GR_GL_FRAGMENT_SHADER, fragShaderSrc)) {
        return false;
    }

    return true;
}

void GrGLShaderBuilder::bindProgramLocations(GrGLuint programId) const {
    if (NULL != fVertexBuilder.get()) {
        fVertexBuilder->bindProgramLocations(programId);
    }

    if (fHasCustomColorOutput) {
        GL_CALL(BindFragDataLocation(programId, 0, declared_color_output_name()));
    }
    if (fHasSecondaryOutput) {
        GL_CALL(BindFragDataLocationIndexed(programId, 0, 1, dual_source_output_name()));
    }
}

const GrGLContextInfo& GrGLShaderBuilder::ctxInfo() const {
    return fGpu->ctxInfo();
}

////////////////////////////////////////////////////////////////////////////

GrGLShaderBuilder::VertexBuilder::VertexBuilder(GrGLShaderBuilder* parent,
                                                GrGpuGL* gpu,
                                                const GrGLProgramDesc& desc)
    : fParent(parent)
    , fGpu(gpu)
    , fDesc(desc)
    , fVSAttrs(kVarsPerBlock)
    , fVSOutputs(kVarsPerBlock)
    , fGSInputs(kVarsPerBlock)
    , fGSOutputs(kVarsPerBlock) {

    const GrGLProgramDesc::KeyHeader& header = fDesc.getHeader();

    fPositionVar = &fVSAttrs.push_back();
    fPositionVar->set(kVec2f_GrSLType, GrGLShaderVar::kAttribute_TypeModifier, "aPosition");
    if (-1 != header.fLocalCoordAttributeIndex) {
        fLocalCoordsVar = &fVSAttrs.push_back();
        fLocalCoordsVar->set(kVec2f_GrSLType,
                             GrGLShaderVar::kAttribute_TypeModifier,
                             "aLocalCoords");
    } else {
        fLocalCoordsVar = fPositionVar;
    }

    const char* viewMName;
    fViewMatrixUniform = fParent->addUniform(GrGLShaderBuilder::kVertex_Visibility,
                                             kMat33f_GrSLType, "ViewM", &viewMName);

    this->vsCodeAppendf("\tvec3 pos3 = %s * vec3(%s, 1);\n"
                        "\tgl_Position = vec4(pos3.xy, 0, pos3.z);\n",
                        viewMName, fPositionVar->c_str());

    // we output point size in the GS if present
    if (header.fEmitsPointSize
#if GR_GL_EXPERIMENTAL_GS
        && !header.fExperimentalGS
#endif
        ) {
        this->vsCodeAppend("\tgl_PointSize = 1.0;\n");
    }
}

bool GrGLShaderBuilder::VertexBuilder::addAttribute(GrSLType type,
                                                    const char* name) {
    for (int i = 0; i < fVSAttrs.count(); ++i) {
        const GrGLShaderVar& attr = fVSAttrs[i];
        // if attribute already added, don't add it again
        if (attr.getName().equals(name)) {
            SkASSERT(attr.getType() == type);
            return false;
        }
    }
    fVSAttrs.push_back().set(type,
                             GrGLShaderVar::kAttribute_TypeModifier,
                             name);
    return true;
}

bool GrGLShaderBuilder::VertexBuilder::addEffectAttribute(int attributeIndex,
                                                          GrSLType type,
                                                          const SkString& name) {
    if (!this->addAttribute(type, name.c_str())) {
        return false;
    }

    fEffectAttributes.push_back().set(attributeIndex, name);
    return true;
}

void GrGLShaderBuilder::VertexBuilder::addVarying(GrSLType type,
                                                  const char* name,
                                                  const char** vsOutName,
                                                  const char** fsInName) {
    fVSOutputs.push_back();
    fVSOutputs.back().setType(type);
    fVSOutputs.back().setTypeModifier(GrGLShaderVar::kVaryingOut_TypeModifier);
    fParent->nameVariable(fVSOutputs.back().accessName(), 'v', name);

    if (vsOutName) {
        *vsOutName = fVSOutputs.back().getName().c_str();
    }
    // input to FS comes either from VS or GS
    const SkString* fsName;
#if GR_GL_EXPERIMENTAL_GS
    if (fDesc.getHeader().fExperimentalGS) {
        // if we have a GS take each varying in as an array
        // and output as non-array.
        fGSInputs.push_back();
        fGSInputs.back().setType(type);
        fGSInputs.back().setTypeModifier(GrGLShaderVar::kVaryingIn_TypeModifier);
        fGSInputs.back().setUnsizedArray();
        *fGSInputs.back().accessName() = fVSOutputs.back().getName();
        fGSOutputs.push_back();
        fGSOutputs.back().setType(type);
        fGSOutputs.back().setTypeModifier(GrGLShaderVar::kVaryingOut_TypeModifier);
        fParent->nameVariable(fGSOutputs.back().accessName(), 'g', name);
        fsName = fGSOutputs.back().accessName();
    } else
#endif
    {
        fsName = fVSOutputs.back().accessName();
    }
    fParent->fsInputAppend().set(type,
                                 GrGLShaderVar::kVaryingIn_TypeModifier,
                                 *fsName);
    if (fsInName) {
        *fsInName = fsName->c_str();
    }
}

const SkString* GrGLShaderBuilder::VertexBuilder::getEffectAttributeName(int attributeIndex) const {
    const AttributePair* attribEnd = fEffectAttributes.end();
    for (const AttributePair* attrib = fEffectAttributes.begin(); attrib != attribEnd; ++attrib) {
        if (attrib->fIndex == attributeIndex) {
            return &attrib->fName;
        }
    }

    return NULL;
}

bool GrGLShaderBuilder::VertexBuilder::compileAndAttachShaders(GrGLuint programId) const {
    SkString vertShaderSrc(GrGetGLSLVersionDecl(fParent->ctxInfo()));
    fParent->appendUniformDecls(kVertex_Visibility, &vertShaderSrc);
    fParent->appendDecls(fVSAttrs, &vertShaderSrc);
    fParent->appendDecls(fVSOutputs, &vertShaderSrc);
    vertShaderSrc.append("void main() {\n");
    vertShaderSrc.append(fVSCode);
    vertShaderSrc.append("}\n");
    if (!attach_shader(fGpu->glInterface(), programId, GR_GL_VERTEX_SHADER, vertShaderSrc)) {
        return false;
    }

#if GR_GL_EXPERIMENTAL_GS
    if (fDesc.getHeader().fExperimentalGS) {
        SkASSERT(fGpu->glslGeneration() >= k150_GrGLSLGeneration);
        SkString geomShaderSrc(GrGetGLSLVersionDecl(fParent->ctxInfo()));
        geomShaderSrc.append("layout(triangles) in;\n"
                             "layout(triangle_strip, max_vertices = 6) out;\n");
        fParent->appendDecls(fGSInputs, &geomShaderSrc);
        fParent->appendDecls(fGSOutputs, &geomShaderSrc);
        geomShaderSrc.append("void main() {\n");
        geomShaderSrc.append("\tfor (int i = 0; i < 3; ++i) {\n"
                             "\t\tgl_Position = gl_in[i].gl_Position;\n");
        if (fDesc.getHeader().fEmitsPointSize) {
            geomShaderSrc.append("\t\tgl_PointSize = 1.0;\n");
        }
        SkASSERT(fGSInputs.count() == fGSOutputs.count());
        for (int i = 0; i < fGSInputs.count(); ++i) {
            geomShaderSrc.appendf("\t\t%s = %s[i];\n",
                                  fGSOutputs[i].getName().c_str(),
                                  fGSInputs[i].getName().c_str());
        }
        geomShaderSrc.append("\t\tEmitVertex();\n"
                             "\t}\n"
                             "\tEndPrimitive();\n");
        geomShaderSrc.append("}\n");
        if (!attach_shader(fGpu->glInterface(), programId, GR_GL_GEOMETRY_SHADER, geomShaderSrc)) {
            return false;
        }
    }
#endif

    return true;
}

void GrGLShaderBuilder::VertexBuilder::bindProgramLocations(GrGLuint programId) const {
    const GrGLProgramDesc::KeyHeader& header = fDesc.getHeader();

    // Bind the attrib locations to same values for all shaders
    SkASSERT(-1 != header.fPositionAttributeIndex);
    GL_CALL(BindAttribLocation(programId,
                               header.fPositionAttributeIndex,
                               fPositionVar->c_str()));
    if (-1 != header.fLocalCoordAttributeIndex) {
        GL_CALL(BindAttribLocation(programId,
                                   header.fLocalCoordAttributeIndex,
                                   fLocalCoordsVar->c_str()));
    }
    if (-1 != header.fColorAttributeIndex) {
        GL_CALL(BindAttribLocation(programId,
                                   header.fColorAttributeIndex,
                                   color_attribute_name()));
    }
    if (-1 != header.fCoverageAttributeIndex) {
        GL_CALL(BindAttribLocation(programId,
                                   header.fCoverageAttributeIndex,
                                   coverage_attribute_name()));
    }

    const AttributePair* attribEnd = fEffectAttributes.end();
    for (const AttributePair* attrib = fEffectAttributes.begin(); attrib != attribEnd; ++attrib) {
         GL_CALL(BindAttribLocation(programId, attrib->fIndex, attrib->fName.c_str()));
    }
}

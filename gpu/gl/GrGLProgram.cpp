/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrGLProgram.h"

#include "GrAllocator.h"
#include "GrEffect.h"
#include "GrDrawEffect.h"
#include "GrGLEffect.h"
#include "GrGpuGL.h"
#include "GrGLShaderVar.h"
#include "GrGLSL.h"
#include "SkXfermode.h"

SK_DEFINE_INST_COUNT(GrGLProgram)

#define GL_CALL(X) GR_GL_CALL(fGpu->glInterface(), X)
#define GL_CALL_RET(R, X) GR_GL_CALL_RET(fGpu->glInterface(), R, X)

GrGLProgram* GrGLProgram::Create(GrGpuGL* gpu,
                                 const GrGLProgramDesc& desc,
                                 const GrEffectStage* colorStages[],
                                 const GrEffectStage* coverageStages[]) {
    GrGLProgram* program = SkNEW_ARGS(GrGLProgram, (gpu, desc, colorStages, coverageStages));
    if (!program->succeeded()) {
        delete program;
        program = NULL;
    }
    return program;
}

GrGLProgram::GrGLProgram(GrGpuGL* gpu,
                         const GrGLProgramDesc& desc,
                         const GrEffectStage* colorStages[],
                         const GrEffectStage* coverageStages[])
: fGpu(gpu)
, fUniformManager(gpu) {
    fDesc = desc;
    fProgramID = 0;

    fDstCopyTexUnit = -1;

    fColor = GrColor_ILLEGAL;
    fColorFilterColor = GrColor_ILLEGAL;

    fColorEffects.reset(desc.numColorEffects());
    fCoverageEffects.reset(desc.numCoverageEffects());

    this->genProgram(colorStages, coverageStages);
}

GrGLProgram::~GrGLProgram() {
    if (fProgramID) {
        GL_CALL(DeleteProgram(fProgramID));
    }
}

void GrGLProgram::abandon() {
    fProgramID = 0;
}

void GrGLProgram::overrideBlend(GrBlendCoeff* srcCoeff,
                                GrBlendCoeff* dstCoeff) const {
    switch (fDesc.getHeader().fCoverageOutput) {
        case GrGLProgramDesc::kModulate_CoverageOutput:
            break;
        // The prog will write a coverage value to the secondary
        // output and the dst is blended by one minus that value.
        case GrGLProgramDesc::kSecondaryCoverage_CoverageOutput:
        case GrGLProgramDesc::kSecondaryCoverageISA_CoverageOutput:
        case GrGLProgramDesc::kSecondaryCoverageISC_CoverageOutput:
            *dstCoeff = (GrBlendCoeff)GrGpu::kIS2C_GrBlendCoeff;
            break;
        case GrGLProgramDesc::kCombineWithDst_CoverageOutput:
            // We should only have set this if the blend was specified as (1, 0)
            SkASSERT(kOne_GrBlendCoeff == *srcCoeff && kZero_GrBlendCoeff == *dstCoeff);
            break;
        default:
            GrCrash("Unexpected coverage output");
            break;
    }
}

namespace {
// given two blend coefficients determine whether the src
// and/or dst computation can be omitted.
inline void need_blend_inputs(SkXfermode::Coeff srcCoeff,
                              SkXfermode::Coeff dstCoeff,
                              bool* needSrcValue,
                              bool* needDstValue) {
    if (SkXfermode::kZero_Coeff == srcCoeff) {
        switch (dstCoeff) {
            // these all read the src
            case SkXfermode::kSC_Coeff:
            case SkXfermode::kISC_Coeff:
            case SkXfermode::kSA_Coeff:
            case SkXfermode::kISA_Coeff:
                *needSrcValue = true;
                break;
            default:
                *needSrcValue = false;
                break;
        }
    } else {
        *needSrcValue = true;
    }
    if (SkXfermode::kZero_Coeff == dstCoeff) {
        switch (srcCoeff) {
            // these all read the dst
            case SkXfermode::kDC_Coeff:
            case SkXfermode::kIDC_Coeff:
            case SkXfermode::kDA_Coeff:
            case SkXfermode::kIDA_Coeff:
                *needDstValue = true;
                break;
            default:
                *needDstValue = false;
                break;
        }
    } else {
        *needDstValue = true;
    }
}

/**
 * Create a blend_coeff * value string to be used in shader code. Sets empty
 * string if result is trivially zero.
 */
inline void blend_term_string(SkString* str, SkXfermode::Coeff coeff,
                       const char* src, const char* dst,
                       const char* value) {
    switch (coeff) {
    case SkXfermode::kZero_Coeff:    /** 0 */
        *str = "";
        break;
    case SkXfermode::kOne_Coeff:     /** 1 */
        *str = value;
        break;
    case SkXfermode::kSC_Coeff:
        str->printf("(%s * %s)", src, value);
        break;
    case SkXfermode::kISC_Coeff:
        str->printf("((%s - %s) * %s)", GrGLSLOnesVecf(4), src, value);
        break;
    case SkXfermode::kDC_Coeff:
        str->printf("(%s * %s)", dst, value);
        break;
    case SkXfermode::kIDC_Coeff:
        str->printf("((%s - %s) * %s)", GrGLSLOnesVecf(4), dst, value);
        break;
    case SkXfermode::kSA_Coeff:      /** src alpha */
        str->printf("(%s.a * %s)", src, value);
        break;
    case SkXfermode::kISA_Coeff:     /** inverse src alpha (i.e. 1 - sa) */
        str->printf("((1.0 - %s.a) * %s)", src, value);
        break;
    case SkXfermode::kDA_Coeff:      /** dst alpha */
        str->printf("(%s.a * %s)", dst, value);
        break;
    case SkXfermode::kIDA_Coeff:     /** inverse dst alpha (i.e. 1 - da) */
        str->printf("((1.0 - %s.a) * %s)", dst, value);
        break;
    default:
        GrCrash("Unexpected xfer coeff.");
        break;
    }
}
/**
 * Adds a line to the fragment shader code which modifies the color by
 * the specified color filter.
 */
void add_color_filter(GrGLShaderBuilder* builder,
                      const char * outputVar,
                      SkXfermode::Coeff uniformCoeff,
                      SkXfermode::Coeff colorCoeff,
                      const char* filterColor,
                      const char* inColor) {
    SkString colorStr, constStr;
    blend_term_string(&colorStr, colorCoeff, filterColor, inColor, inColor);
    blend_term_string(&constStr, uniformCoeff, filterColor, inColor, filterColor);

    SkString sum;
    GrGLSLAddf<4>(&sum, colorStr.c_str(), constStr.c_str());
    builder->fsCodeAppendf("\t%s = %s;\n", outputVar, sum.c_str());
}
}

namespace {

void expand_known_value4f(SkString* string, GrSLConstantVec vec) {
    SkASSERT(string->isEmpty() == (vec != kNone_GrSLConstantVec));
    switch (vec) {
        case kNone_GrSLConstantVec:
            break;
        case kZeros_GrSLConstantVec:
            *string = GrGLSLZerosVecf(4);
            break;
        case kOnes_GrSLConstantVec:
            *string = GrGLSLOnesVecf(4);
            break;
    }
}

}

bool GrGLProgram::genProgram(const GrEffectStage* colorStages[],
                             const GrEffectStage* coverageStages[]) {
    SkASSERT(0 == fProgramID);

    const GrGLProgramDesc::KeyHeader& header = fDesc.getHeader();

    bool needsVertexShader = true;

    GrGLShaderBuilder builder(fGpu, fUniformManager, fDesc, needsVertexShader);
    if (GrGLShaderBuilder::VertexBuilder* vertexBuilder = builder.getVertexBuilder()) {
        fUniformHandles.fViewMatrixUni = vertexBuilder->getViewMatrixUniform();
    }

    // incoming color to current stage being processed.
    SkString inColor = builder.getInputColor();
    GrSLConstantVec knownColorValue = builder.getKnownColorValue();

    // Get the coeffs for the Mode-based color filter, determine if color is needed.
    SkXfermode::Coeff colorCoeff;
    SkXfermode::Coeff filterColorCoeff;
    SkAssertResult(
        SkXfermode::ModeAsCoeff(header.fColorFilterXfermode,
                                &filterColorCoeff,
                                &colorCoeff));
    bool needColor, needFilterColor;
    need_blend_inputs(filterColorCoeff, colorCoeff, &needFilterColor, &needColor);

    // used in order for builder to return the per-stage uniform handles.
    typedef SkTArray<GrGLUniformManager::UniformHandle, true>* UniHandleArrayPtr;
    int maxColorOrCovEffectCnt = GrMax(fDesc.numColorEffects(), fDesc.numCoverageEffects());
    SkAutoTArray<UniHandleArrayPtr> effectUniformArrays(maxColorOrCovEffectCnt);
    SkAutoTArray<GrGLEffect*> glEffects(maxColorOrCovEffectCnt);

    if (needColor) {
        for (int e = 0; e < fDesc.numColorEffects(); ++e) {
            effectUniformArrays[e] = &fColorEffects[e].fSamplerUnis;
        }

        builder.emitEffects(colorStages,
                            fDesc.effectKeys(),
                            fDesc.numColorEffects(),
                            &inColor,
                            &knownColorValue,
                            effectUniformArrays.get(),
                            glEffects.get());

        for (int e = 0; e < fDesc.numColorEffects(); ++e) {
            fColorEffects[e].fGLEffect = glEffects[e];
        }
    }

    // Insert the color filter. This will soon be replaced by a color effect.
    if (SkXfermode::kDst_Mode != header.fColorFilterXfermode) {
        const char* colorFilterColorUniName = NULL;
        fUniformHandles.fColorFilterUni = builder.addUniform(GrGLShaderBuilder::kFragment_Visibility,
                                                             kVec4f_GrSLType, "FilterColor",
                                                             &colorFilterColorUniName);

        builder.fsCodeAppend("\tvec4 filteredColor;\n");
        const char* color;
        // add_color_filter requires a real input string.
        if (knownColorValue == kOnes_GrSLConstantVec) {
            color = GrGLSLOnesVecf(4);
        } else if (knownColorValue == kZeros_GrSLConstantVec) {
            color = GrGLSLZerosVecf(4);
        } else {
            color = inColor.c_str();
        }
        add_color_filter(&builder, "filteredColor", filterColorCoeff,
                         colorCoeff, colorFilterColorUniName, color);
        inColor = "filteredColor";
    }

    ///////////////////////////////////////////////////////////////////////////
    // compute the partial coverage
    SkString inCoverage = builder.getInputCoverage();
    GrSLConstantVec knownCoverageValue = builder.getKnownCoverageValue();

    for (int e = 0; e < fDesc.numCoverageEffects(); ++e) {
        effectUniformArrays[e] = &fCoverageEffects[e].fSamplerUnis;
    }

    builder.emitEffects(coverageStages,
                        fDesc.getEffectKeys() + fDesc.numColorEffects(),
                        fDesc.numCoverageEffects(),
                        &inCoverage,
                        &knownCoverageValue,
                        effectUniformArrays.get(),
                        glEffects.get());
    for (int e = 0; e < fDesc.numCoverageEffects(); ++e) {
        fCoverageEffects[e].fGLEffect = glEffects[e];
    }

    // discard if coverage is zero
    if (header.fDiscardIfZeroCoverage && kOnes_GrSLConstantVec != knownCoverageValue) {
        if (kZeros_GrSLConstantVec == knownCoverageValue) {
            // This is unfortunate.
            builder.fsCodeAppend("\tdiscard;\n");
        } else {
            builder.fsCodeAppendf("\tif (all(lessThanEqual(%s, vec4(0.0)))) {\n\t\tdiscard;\n\t}\n",
                                  inCoverage.c_str());
        }
    }

    if (GrGLProgramDesc::CoverageOutputUsesSecondaryOutput(header.fCoverageOutput)) {
        const char* secondaryOutputName = builder.enableSecondaryOutput();

        // default coeff to ones for kCoverage_DualSrcOutput
        SkString coeff;
        GrSLConstantVec knownCoeffValue = kOnes_GrSLConstantVec;
        if (GrGLProgramDesc::kSecondaryCoverageISA_CoverageOutput == header.fCoverageOutput) {
            // Get (1-A) into coeff
            SkString inColorAlpha;
            GrGLSLGetComponent4f(&inColorAlpha,
                                    inColor.c_str(),
                                    kA_GrColorComponentFlag,
                                    knownColorValue,
                                    true);
            knownCoeffValue = GrGLSLSubtractf<1>(&coeff,
                                                 NULL,
                                                 inColorAlpha.c_str(),
                                                 kOnes_GrSLConstantVec,
                                                 knownColorValue,
                                                 true);
        } else if (GrGLProgramDesc::kSecondaryCoverageISC_CoverageOutput == header.fCoverageOutput) {
            // Get (1-RGBA) into coeff
            knownCoeffValue = GrGLSLSubtractf<4>(&coeff,
                                                 NULL,
                                                 inColor.c_str(),
                                                 kOnes_GrSLConstantVec,
                                                 knownColorValue,
                                                 true);
        }
        // Get coeff * coverage into modulate and then write that to the dual source output.
        SkString modulate;
        GrGLSLModulatef<4>(&modulate,
                           coeff.c_str(),
                           inCoverage.c_str(),
                           knownCoeffValue,
                           knownCoverageValue,
                           false);
        builder.fsCodeAppendf("\t%s = %s;\n", secondaryOutputName, modulate.c_str());
    }

    ///////////////////////////////////////////////////////////////////////////
    // combine color and coverage as frag color

    // Get "color * coverage" into fragColor
    SkString fragColor;
    GrSLConstantVec knownFragColorValue = GrGLSLModulatef<4>(&fragColor,
                                                             inColor.c_str(),
                                                             inCoverage.c_str(),
                                                             knownColorValue,
                                                             knownCoverageValue,
                                                             true);
    // Now tack on "+(1-coverage)dst onto the frag color if we were asked to do so.
    if (GrGLProgramDesc::kCombineWithDst_CoverageOutput == header.fCoverageOutput) {
        SkString dstCoeff;
        GrSLConstantVec knownDstCoeffValue = GrGLSLSubtractf<4>(&dstCoeff,
                                                                NULL,
                                                                inCoverage.c_str(),
                                                                kOnes_GrSLConstantVec,
                                                                knownCoverageValue,
                                                                true);
        SkString dstContribution;
        GrSLConstantVec knownDstContributionValue = GrGLSLModulatef<4>(&dstContribution,
                                                                       dstCoeff.c_str(),
                                                                       builder.dstColor(),
                                                                       knownDstCoeffValue,
                                                                       kNone_GrSLConstantVec,
                                                                       true);
        SkString oldFragColor = fragColor;
        fragColor.reset();
        GrGLSLAddf<4>(&fragColor,
                      oldFragColor.c_str(),
                      dstContribution.c_str(),
                      knownFragColorValue,
                      knownDstContributionValue,
                      false);
    } else {
        expand_known_value4f(&fragColor, knownFragColorValue);
    }
    builder.fsCodeAppendf("\t%s = %s;\n", builder.getColorOutputName(), fragColor.c_str());

    if (!builder.finish(&fProgramID)) {
        return false;
    }

    fUniformHandles.fRTHeightUni = builder.getRTHeightUniform();
    fUniformHandles.fDstCopyTopLeftUni = builder.getDstCopyTopLeftUniform();
    fUniformHandles.fDstCopyScaleUni = builder.getDstCopyScaleUniform();
    fUniformHandles.fColorUni = builder.getColorUniform();
    fUniformHandles.fCoverageUni = builder.getCoverageUniform();
    fUniformHandles.fDstCopySamplerUni = builder.getDstCopySamplerUniform();
    // This must be called after we set fDstCopySamplerUni above.
    this->initSamplerUniforms();

    return true;
}

void GrGLProgram::initSamplerUniforms() {
    GL_CALL(UseProgram(fProgramID));
    GrGLint texUnitIdx = 0;
    if (fUniformHandles.fDstCopySamplerUni.isValid()) {
        fUniformManager.setSampler(fUniformHandles.fDstCopySamplerUni, texUnitIdx);
        fDstCopyTexUnit = texUnitIdx++;
    }

    for (int e = 0; e < fColorEffects.count(); ++e) {
        this->initEffectSamplerUniforms(&fColorEffects[e], &texUnitIdx);
    }

    for (int e = 0; e < fCoverageEffects.count(); ++e) {
        this->initEffectSamplerUniforms(&fCoverageEffects[e], &texUnitIdx);
    }
}

void GrGLProgram::initEffectSamplerUniforms(EffectAndSamplers* effect, int* texUnitIdx) {
    int numSamplers = effect->fSamplerUnis.count();
    effect->fTextureUnits.reset(numSamplers);
    for (int s = 0; s < numSamplers; ++s) {
        UniformHandle handle = effect->fSamplerUnis[s];
        if (handle.isValid()) {
            fUniformManager.setSampler(handle, *texUnitIdx);
            effect->fTextureUnits[s] = (*texUnitIdx)++;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

void GrGLProgram::setEffectData(const GrEffectStage& stage,
                                const EffectAndSamplers& effect) {

    // Let the GrGLEffect set its data.
    bool explicitLocalCoords = -1 != fDesc.getHeader().fLocalCoordAttributeIndex;
    GrDrawEffect drawEffect(stage, explicitLocalCoords);
    effect.fGLEffect->setData(fUniformManager, drawEffect);

    // Bind the texures for the effect.
    int numSamplers = effect.fSamplerUnis.count();
    SkASSERT((*stage.getEffect())->numTextures() == numSamplers);
    for (int s = 0; s < numSamplers; ++s) {
        UniformHandle handle = effect.fSamplerUnis[s];
        if (handle.isValid()) {
            const GrTextureAccess& access = (*stage.getEffect())->textureAccess(s);
            GrGLTexture* texture = static_cast<GrGLTexture*>(access.getTexture());
            int unit = effect.fTextureUnits[s];
            fGpu->bindTexture(unit, access.getParams(), texture);
        }
    }
}

void GrGLProgram::setData(GrDrawState::BlendOptFlags blendOpts,
                          const GrEffectStage* colorStages[],
                          const GrEffectStage* coverageStages[],
                          const GrDeviceCoordTexture* dstCopy,
                          SharedGLState* sharedState) {
    const GrDrawState& drawState = fGpu->getDrawState();

    GrColor color;
    GrColor coverage;
    if (blendOpts & GrDrawState::kEmitTransBlack_BlendOptFlag) {
        color = 0;
        coverage = 0;
    } else if (blendOpts & GrDrawState::kEmitCoverage_BlendOptFlag) {
        color = 0xffffffff;
        coverage = drawState.getCoverage();
    } else {
        color = drawState.getColor();
        coverage = drawState.getCoverage();
    }

    this->setColor(drawState, color, sharedState);
    this->setCoverage(drawState, coverage, sharedState);
    this->setMatrixAndRenderTargetHeight(drawState);

    // Setup the SkXfermode::Mode-based colorfilter uniform if necessary
    if (fUniformHandles.fColorFilterUni.isValid() &&
        fColorFilterColor != drawState.getColorFilterColor()) {
        GrGLfloat c[4];
        GrColorToRGBAFloat(drawState.getColorFilterColor(), c);
        fUniformManager.set4fv(fUniformHandles.fColorFilterUni, 0, 1, c);
        fColorFilterColor = drawState.getColorFilterColor();
    }

    if (NULL != dstCopy) {
        if (fUniformHandles.fDstCopyTopLeftUni.isValid()) {
            fUniformManager.set2f(fUniformHandles.fDstCopyTopLeftUni,
                                  static_cast<GrGLfloat>(dstCopy->offset().fX),
                                  static_cast<GrGLfloat>(dstCopy->offset().fY));
            fUniformManager.set2f(fUniformHandles.fDstCopyScaleUni,
                                  1.f / dstCopy->texture()->width(),
                                  1.f / dstCopy->texture()->height());
            GrGLTexture* texture = static_cast<GrGLTexture*>(dstCopy->texture());
            static GrTextureParams kParams; // the default is clamp, nearest filtering.
            fGpu->bindTexture(fDstCopyTexUnit, kParams, texture);
        } else {
            SkASSERT(!fUniformHandles.fDstCopyScaleUni.isValid());
            SkASSERT(!fUniformHandles.fDstCopySamplerUni.isValid());
        }
    } else {
        SkASSERT(!fUniformHandles.fDstCopyTopLeftUni.isValid());
        SkASSERT(!fUniformHandles.fDstCopyScaleUni.isValid());
        SkASSERT(!fUniformHandles.fDstCopySamplerUni.isValid());
    }

    for (int e = 0; e < fColorEffects.count(); ++e) {
        // We may have omitted the GrGLEffect because of the color filter logic in genProgram.
        // This can be removed when the color filter is an effect.
        if (NULL != fColorEffects[e].fGLEffect) {
            this->setEffectData(*colorStages[e], fColorEffects[e]);
        }
    }

    for (int e = 0; e < fCoverageEffects.count(); ++e) {
        if (NULL != fCoverageEffects[e].fGLEffect) {
            this->setEffectData(*coverageStages[e], fCoverageEffects[e]);
        }
    }
}

void GrGLProgram::setColor(const GrDrawState& drawState,
                           GrColor color,
                           SharedGLState* sharedState) {
    const GrGLProgramDesc::KeyHeader& header = fDesc.getHeader();
    if (!drawState.hasColorVertexAttribute()) {
        switch (header.fColorInput) {
            case GrGLProgramDesc::kAttribute_ColorInput:
                SkASSERT(-1 != header.fColorAttributeIndex);
                if (sharedState->fConstAttribColor != color ||
                    sharedState->fConstAttribColorIndex != header.fColorAttributeIndex) {
                    // OpenGL ES only supports the float varieties of glVertexAttrib
                    GrGLfloat c[4];
                    GrColorToRGBAFloat(color, c);
                    GL_CALL(VertexAttrib4fv(header.fColorAttributeIndex, c));
                    sharedState->fConstAttribColor = color;
                    sharedState->fConstAttribColorIndex = header.fColorAttributeIndex;
                }
                break;
            case GrGLProgramDesc::kUniform_ColorInput:
                if (fColor != color) {
                    // OpenGL ES doesn't support unsigned byte varieties of glUniform
                    GrGLfloat c[4];
                    GrColorToRGBAFloat(color, c);
                    fUniformManager.set4fv(fUniformHandles.fColorUni, 0, 1, c);
                    fColor = color;
                }
                sharedState->fConstAttribColorIndex = -1;
                break;
            case GrGLProgramDesc::kSolidWhite_ColorInput:
            case GrGLProgramDesc::kTransBlack_ColorInput:
                sharedState->fConstAttribColorIndex = -1;
                break;
            default:
                GrCrash("Unknown color type.");
        }
    } else {
        sharedState->fConstAttribColorIndex = -1;
    }
}

void GrGLProgram::setCoverage(const GrDrawState& drawState,
                              GrColor coverage,
                              SharedGLState* sharedState) {
    const GrGLProgramDesc::KeyHeader& header = fDesc.getHeader();
    if (!drawState.hasCoverageVertexAttribute()) {
        switch (header.fCoverageInput) {
            case GrGLProgramDesc::kAttribute_ColorInput:
                if (sharedState->fConstAttribCoverage != coverage ||
                    sharedState->fConstAttribCoverageIndex != header.fCoverageAttributeIndex) {
                    // OpenGL ES only supports the float varieties of  glVertexAttrib
                    GrGLfloat c[4];
                    GrColorToRGBAFloat(coverage, c);
                    GL_CALL(VertexAttrib4fv(header.fCoverageAttributeIndex, c));
                    sharedState->fConstAttribCoverage = coverage;
                    sharedState->fConstAttribCoverageIndex = header.fCoverageAttributeIndex;
                }
                break;
            case GrGLProgramDesc::kUniform_ColorInput:
                if (fCoverage != coverage) {
                    // OpenGL ES doesn't support unsigned byte varieties of glUniform
                    GrGLfloat c[4];
                    GrColorToRGBAFloat(coverage, c);
                    fUniformManager.set4fv(fUniformHandles.fCoverageUni, 0, 1, c);
                    fCoverage = coverage;
                }
                sharedState->fConstAttribCoverageIndex = -1;
                break;
            case GrGLProgramDesc::kSolidWhite_ColorInput:
            case GrGLProgramDesc::kTransBlack_ColorInput:
                sharedState->fConstAttribCoverageIndex = -1;
                break;
            default:
                GrCrash("Unknown coverage type.");
        }
    } else {
        sharedState->fConstAttribCoverageIndex = -1;
    }
}

void GrGLProgram::setMatrixAndRenderTargetHeight(const GrDrawState& drawState) {
    const GrRenderTarget* rt = drawState.getRenderTarget();
    SkISize size;
    size.set(rt->width(), rt->height());

    // Load the RT height uniform if it is needed to y-flip gl_FragCoord.
    if (fUniformHandles.fRTHeightUni.isValid() &&
        fMatrixState.fRenderTargetSize.fHeight != size.fHeight) {
        fUniformManager.set1f(fUniformHandles.fRTHeightUni, SkIntToScalar(size.fHeight));
    }

    if (fMatrixState.fRenderTargetOrigin != rt->origin() ||
        !fMatrixState.fViewMatrix.cheapEqualTo(drawState.getViewMatrix()) ||
        fMatrixState.fRenderTargetSize != size) {

        fMatrixState.fViewMatrix = drawState.getViewMatrix();
        fMatrixState.fRenderTargetSize = size;
        fMatrixState.fRenderTargetOrigin = rt->origin();

        GrGLfloat viewMatrix[3 * 3];
        fMatrixState.getGLMatrix<3>(viewMatrix);
        fUniformManager.setMatrix3f(fUniformHandles.fViewMatrixUni, viewMatrix);
    }
}

/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * This file was autogenerated from GrUnpremulInputFragmentProcessor.fp; do not modify.
 */
#include "GrUnpremulInputFragmentProcessor.h"
#if SK_SUPPORT_GPU
#include "glsl/GrGLSLFragmentProcessor.h"
#include "glsl/GrGLSLFragmentShaderBuilder.h"
#include "glsl/GrGLSLProgramBuilder.h"
#include "GrTexture.h"
#include "SkSLCPP.h"
#include "SkSLUtil.h"
class GrGLSLUnpremulInputFragmentProcessor : public GrGLSLFragmentProcessor {
public:
    GrGLSLUnpremulInputFragmentProcessor() {}
    void emitCode(EmitArgs& args) override {
        GrGLSLFPFragmentBuilder* fragBuilder = args.fFragBuilder;
        const GrUnpremulInputFragmentProcessor& _outer =
                args.fFp.cast<GrUnpremulInputFragmentProcessor>();
        (void)_outer;
        fragBuilder->codeAppendf(
                "%s = %s;\nhalf invAlpha = %s.w <= 0.0 ? 0.0 : 1.0 / %s.w;\n%s.xyz *= invAlpha;\n",
                args.fOutputColor, args.fInputColor ? args.fInputColor : "half4(1)",
                args.fInputColor ? args.fInputColor : "half4(1)",
                args.fInputColor ? args.fInputColor : "half4(1)", args.fOutputColor);
    }

private:
    void onSetData(const GrGLSLProgramDataManager& pdman,
                   const GrFragmentProcessor& _proc) override {}
};
GrGLSLFragmentProcessor* GrUnpremulInputFragmentProcessor::onCreateGLSLInstance() const {
    return new GrGLSLUnpremulInputFragmentProcessor();
}
void GrUnpremulInputFragmentProcessor::onGetGLSLProcessorKey(const GrShaderCaps& caps,
                                                             GrProcessorKeyBuilder* b) const {}
bool GrUnpremulInputFragmentProcessor::onIsEqual(const GrFragmentProcessor& other) const {
    const GrUnpremulInputFragmentProcessor& that = other.cast<GrUnpremulInputFragmentProcessor>();
    (void)that;
    return true;
}
GrUnpremulInputFragmentProcessor::GrUnpremulInputFragmentProcessor(
        const GrUnpremulInputFragmentProcessor& src)
        : INHERITED(kGrUnpremulInputFragmentProcessor_ClassID, src.optimizationFlags()) {}
std::unique_ptr<GrFragmentProcessor> GrUnpremulInputFragmentProcessor::clone() const {
    return std::unique_ptr<GrFragmentProcessor>(new GrUnpremulInputFragmentProcessor(*this));
}
#endif

#pragma once
#include "maga_transformer/cpp/normal_engine/NormalBatchStreamProcessor.h"
namespace rtp_llm {
class ScoreBatchStreamProcessor: public NormalBatchStreamProcessor {
public:
    ScoreBatchStreamProcessor(const ft::GptInitParameter& params): NormalBatchStreamProcessor(params) {}
    virtual absl::Status dispatch(const StreamGroups& stream_groups, const MergedOutput& merge_outputs) const override;
    virtual absl::StatusOr<GptModelInputs> gatherModelInput(const StreamGroups& stream_groups) const override;
    virtual absl::StatusOr<SamplerInputs>  gatherSamplerInput(const StreamGroups&    stream_groups,
                                                              const GptModelInputs&  model_inputs,
                                                              const GptModelOutputs& model_output) const override;
};
}  // namespace rtp_llm
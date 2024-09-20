#include <cstdlib>

#include "src/fastertransformer/core/Types.h"
#include "src/fastertransformer/devices/testing/TestBase.h"
#include "maga_transformer/cpp/speculative_engine/test/SpeculativeMockEngine.h"
#include "gmock/gmock-actions.h"
#include "gmock/gmock-function-mocker.h"
#include "gtest/gtest.h"
#include <memory>

using namespace std;
namespace W  = ft::W;
namespace ft = fastertransformer;
namespace rtp_llm {

class SpeculativeNormalEngineTest: public DeviceTestBase {
public:

};


TEST_F(SpeculativeNormalEngineTest, testSimple) {
    CustomConfig config;
    auto gpt_init_params = ft::GptInitParameter();
    auto engine = createVanillaSpeculativeEngine(device_, config, gpt_init_params);

    ASSERT_TRUE(engine->resourceContext().cache_manager);
    ASSERT_FALSE(engine->resourceContext().system_prompt);
    ASSERT_FALSE(engine->resourceContext().reuse_cache);
    ASSERT_EQ(engine->resourceContext().cache_manager->freeBlockNums(), 99);

    // test streaming query
    {
        std::shared_ptr<GenerateInput> query   = make_shared<GenerateInput>();
        query->input_ids                       = createBuffer<int32_t>({7}, {1, 2, 3, 4, 5, 6, 7}, ft::AllocationType::HOST);
        query->generate_config                 = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens = 3;
        query->generate_config->is_streaming   = true;

        shared_ptr<GenerateStream> stream      = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output1 = stream->nextOutput();
        ASSERT_TRUE(output1.ok());
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.output_len, 1);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.input_len, 7);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.iter_count, 1);

        auto output2 = stream->nextOutput();
        ASSERT_TRUE(output2.ok());
        ASSERT_EQ(output2.value().generate_outputs[0].aux_info.output_len, 3);
        ASSERT_EQ(output2.value().generate_outputs[0].aux_info.input_len, 7);
        ASSERT_EQ(output2.value().generate_outputs[0].aux_info.iter_count, 2);


        ASSERT_TRUE(stream->finished());
        auto output3 = stream->nextOutput();
        ASSERT_TRUE(!output3.ok());
    }

    // test non-streaming query
    {
        std::shared_ptr<GenerateInput> query   = make_shared<GenerateInput>();
        query->input_ids                       = createBuffer<int32_t>({7}, {1, 2, 3, 4, 5, 6, 7}, ft::AllocationType::HOST);
        query->generate_config                 = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens = 5;
        query->generate_config->is_streaming   = false;

        shared_ptr<GenerateStream> stream      = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output = stream->nextOutput();
        ASSERT_TRUE(output.ok());
        ASSERT_EQ(output.value().generate_outputs[0].aux_info.output_len, 5);
        ASSERT_EQ(output.value().generate_outputs[0].aux_info.input_len, 7);
        ASSERT_EQ(output.value().generate_outputs[0].aux_info.iter_count, 3);

        ASSERT_TRUE(stream->finished());
        auto output2 = stream->nextOutput();
        ASSERT_TRUE(!output2.ok());
    }
}



TEST_F(SpeculativeNormalEngineTest, testSystemPrompt) {
    CustomConfig config;
    vector<int> prompt_1 = {1, 2, 3};
    vector<int> prompt_2 = {4, 5, 6, 7, 8, 9};
    config.multi_task_prompt_tokens = {{"1", prompt_1}, {"2", prompt_2}};
    auto gpt_init_params = ft::GptInitParameter();
    auto engine = createVanillaSpeculativeEngine(device_, config, gpt_init_params);
    ASSERT_TRUE(engine->resourceContext().cache_manager);
    ASSERT_TRUE(engine->resourceContext().system_prompt);
    ASSERT_TRUE(engine->resourceContext().reuse_cache);
    ASSERT_EQ(engine->resourceContext().cache_manager->freeBlockNums(), 96);

    {
        std::shared_ptr<GenerateInput> query   = make_shared<GenerateInput>();
        query->input_ids                       = createBuffer<int32_t>({7}, {1, 2, 3, 4, 5, 6, 7}, ft::AllocationType::HOST);
        query->generate_config                 = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens = 1;
        shared_ptr<GenerateStream> stream      = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output1 = stream->nextOutput();
        ASSERT_TRUE(output1.ok());
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.output_len, 1);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.prefix_len, 0);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.reuse_len, 2);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.input_len, 7);

        ASSERT_TRUE(stream->finished());
        auto output2 = stream->nextOutput();
        ASSERT_TRUE(!output2.ok());
    }
    {
        std::shared_ptr<GenerateInput> query    = make_shared<GenerateInput>();
        query->input_ids                        = createBuffer<int32_t>({7}, {10, 20, 30, 40, 50, 60, 70}, ft::AllocationType::HOST);
        query->generate_config                  = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens  = 1;
        shared_ptr<GenerateStream> stream       = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output1 = stream->nextOutput();
        ASSERT_TRUE(output1.ok());
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.output_len, 1);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.prefix_len, 0);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.reuse_len, 0);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.input_len, 7);

        ASSERT_TRUE(stream->finished());
        auto output2 = stream->nextOutput();
        ASSERT_TRUE(!output2.ok());
    }
    {
        std::shared_ptr<GenerateInput> query    = make_shared<GenerateInput>();
        query->input_ids                        = createBuffer<int32_t>({7}, {10, 20, 30, 40, 50, 60, 70}, ft::AllocationType::HOST);
        query->generate_config                  = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens  = 1;
        query->generate_config->task_id         = "2";
        shared_ptr<GenerateStream> stream       = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output1 = stream->nextOutput();
        ASSERT_TRUE(output1.ok());
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.output_len, 1);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.prefix_len, 6);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.reuse_len, 4);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.input_len, 7);

        ASSERT_TRUE(stream->finished());
        auto output2 = stream->nextOutput();
        ASSERT_TRUE(!output2.ok());
    }
}


TEST_F(SpeculativeNormalEngineTest, testReuseCache) {
    CustomConfig config;
    config.reuse_cache = true;
    auto gpt_init_params = ft::GptInitParameter();
    auto engine = createVanillaSpeculativeEngine(device_, config, gpt_init_params);
    ASSERT_TRUE(engine->resourceContext().reuse_cache);
    ASSERT_EQ(engine->resourceContext().cache_manager->freeBlockNums(), 99);
    {
        std::shared_ptr<GenerateInput> query   = make_shared<GenerateInput>();
        query->input_ids                       = createBuffer<int32_t>({7}, {1, 2, 3, 4, 5, 6, 7}, ft::AllocationType::HOST);
        query->generate_config                 = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens = 1;
        shared_ptr<GenerateStream> stream      = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output1 = stream->nextOutput();
        ASSERT_TRUE(output1.ok());
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.output_len, 1);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.prefix_len, 0);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.reuse_len, 0);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.input_len, 7);

        ASSERT_TRUE(stream->finished());
        auto output2 = stream->nextOutput();
        ASSERT_TRUE(!output2.ok());
    }

    {
        std::shared_ptr<GenerateInput> query   = make_shared<GenerateInput>();
        query->input_ids                       = createBuffer<int32_t>({7}, {1, 2, 3, 4, 50, 60, 70}, ft::AllocationType::HOST);
        query->generate_config                 = make_shared<GenerateConfig>();
        query->generate_config->max_new_tokens = 1;
        shared_ptr<GenerateStream> stream      = engine->enqueue(query);

        ASSERT_TRUE(stream != nullptr);
        auto output1 = stream->nextOutput();
        ASSERT_TRUE(output1.ok());
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.output_len, 1);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.prefix_len, 0);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.reuse_len, 4);
        ASSERT_EQ(output1.value().generate_outputs[0].aux_info.input_len, 7);

        ASSERT_TRUE(stream->finished());
        auto output2 = stream->nextOutput();
        ASSERT_TRUE(!output2.ok());
    }
}

}  // namespace rtp_llm

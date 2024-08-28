#include "maga_transformer/cpp/HttpApiServer.h"
#include "autil/AtomicCounter.h"
#include "autil/legacy/json.h"
#include "autil/legacy/jsonizable.h"
#include "src/fastertransformer/utils/py_utils/pybind_utils.h"

namespace rtp_llm {

using namespace std::placeholders;
using namespace autil::legacy;
using namespace autil::legacy::json;

class AuxInfoAdapter : public Jsonizable, public AuxInfo {
public:
    void Jsonize(Jsonizable::JsonWrapper& json) override {
        json.Jsonize("cost_time_ms", cost_time_ms, cost_time_ms);
        json.Jsonize("iter_count", iter_count, iter_count);
        json.Jsonize("input_len", input_len, input_len);
        json.Jsonize("prefix_len", prefix_len, prefix_len);
        json.Jsonize("reuse_len", reuse_len, reuse_len);
        json.Jsonize("output_len", output_len, output_len);
        json.Jsonize("fallback_tokens", fallback_tokens, fallback_tokens);
        json.Jsonize("fallback_times", fallback_times, fallback_times);
    }
    AuxInfoAdapter() {
        AuxInfo();
    }
    AuxInfoAdapter(const AuxInfo& base) {
        cost_time_us    = base.cost_time_us;
        iter_count      = base.iter_count;
        input_len       = base.input_len;
        prefix_len      = base.prefix_len;
        reuse_len       = base.reuse_len;
        output_len      = base.output_len;
        fallback_tokens = base.fallback_tokens;
        fallback_times  = base.fallback_times;

        cost_time_ms = cost_time_us / 1000.0;
    }
    float cost_time_ms;
};

struct PipelineResponse : public Jsonizable {
    void Jsonize(Jsonizable::JsonWrapper& json) override {
        json.Jsonize("response", response, response);
        json.Jsonize("finished", finished, finished);
        json.Jsonize("aux_info", aux_info, aux_info);
    }
    std::string response;
    bool finished;
    AuxInfoAdapter aux_info;
};

autil::AtomicCounter requestCounter;

void inferResponse(std::unique_ptr<http_server::HttpResponseWriter> writer,
                   const http_server::HttpRequest&                  request,
                   std::shared_ptr<EngineBase>                      engine,
                   ft::GptInitParameter                             params,
                   Pipeline                                         pipeline_) {

    std::shared_ptr<GenerateInput> input = std::make_shared<GenerateInput>();
    input->request_id = requestCounter.incAndReturn();
    input->begin_time_ms = autil::TimeUtility::currentTimeInMicroSeconds();
    input->generate_config = std::make_shared<GenerateConfig>();

    auto body = ParseJson(request.getBody());
    auto bodyMap = AnyCast<JsonMap>(body);

    // generate_config
    auto it = bodyMap.find("generate_config");
    if (it == bodyMap.end()) {
        FT_LOG_INFO("no generate_config in http request.");
    } else {
        FromJson(input->generate_config, it->second);
    }

    // merge stop_words_list
    for (const auto& innerVec : params.special_tokens_.stop_words_list_) {
        std::vector<int> tmpVec;
        for (int64_t val : innerVec) {
            tmpVec.push_back(static_cast<int>(val));
        }
        input->generate_config->stop_words_list.push_back(tmpVec);
    }

    // merge stop_words_str
    std::vector<std::string> stop_words_str;
    auto generate_config_map = AnyCast<JsonMap>(it->second);
    it = generate_config_map.find("stop_words_str");
    if (it == generate_config_map.end()) {
        FT_LOG_INFO("no stop_words_str in http request.");
    } else {
        auto words = AnyCast<JsonArray>(it->second);
        for (auto word : words) {
            stop_words_str.push_back(AnyCast<std::string>(word));
        }
    }
    stop_words_str.insert(stop_words_str.begin(),
                          params.special_tokens_.stop_words_str_.begin(),
                          params.special_tokens_.stop_words_str_.end());

    // urls/images: list[list[str]]
    it = bodyMap.find("urls");
    if (it != bodyMap.end()) {
        auto listListUrls = AnyCast<JsonArray>(it->second);
        auto listUrls = AnyCast<JsonArray>(listListUrls[0]);
        std::vector<std::string> mm_urls;
        for (auto url : listUrls) {
            mm_urls.emplace_back(AnyCast<std::string>(url));
        }
        input->multimodal_urls = std::move(mm_urls);
    } else {
        FT_LOG_INFO("no urls in http request.");
        it = bodyMap.find("images");
        if (it != bodyMap.end()) {
            auto listListUrls = AnyCast<JsonArray>(it->second);
            auto listUrls = AnyCast<JsonArray>(listListUrls[0]);
            std::vector<std::string> mm_urls;
            for (auto url : listUrls) {
                mm_urls.emplace_back(AnyCast<std::string>(url));
            }
            input->multimodal_urls = std::move(mm_urls);
        } else {
            FT_LOG_INFO("no images in http request." );
        }
    }

    it = bodyMap.find("prompt");
    std::string prompt = "hello";
    if (it == bodyMap.end()) {
        FT_LOG_INFO("no prompt in http request.");
    } else {
        prompt = AnyCast<std::string>(it->second);
    }

    auto vec = pipeline_.encode(prompt);

    auto device = ft::DeviceFactory::getDefaultDevice();
    input->input_ids = device->allocateBuffer(
        {ft::DataType::TYPE_INT32, {vec.size()}, ft::AllocationType::HOST}, {});
    memcpy(input->input_ids->data(), vec.data(), input->input_ids->sizeBytes());

    auto stream = engine->enqueue(input);
    while (!stream->finished()) {
        const auto output_status = stream->nextOutput();
        if (!output_status.ok()) { break; }
        const GenerateOutputs *responses = &(output_status.value());

        for (size_t i = 0; i < responses->generate_outputs.size(); i++) {
            const auto& response = responses->generate_outputs[i];
            const ft::Buffer *buffer = response.output_ids.get();
            int32_t *output_ids = reinterpret_cast<int32_t*>(buffer->data());
            std::vector<int32_t> tokens;
            for (size_t i = 0; i < buffer->size(); i++) {
                tokens.emplace_back(output_ids[i]);
            }

            // TODO
            //auto genenate_texts = Pipeline::decode_tokens(tokens, stop_words_str, finished, token_buffer, incremental, print_stop_words);
            auto generate_texts = pipeline_.decode(tokens);
            auto json_response = Pipeline::format_response(generate_texts, responses);
            if (input->generate_config->is_streaming) {
                auto sse_response = HttpApiServer::SseResponse(json_response);
                writer->AddHeader("Content-Type", "text/event-stream");
                writer->WriteStream(sse_response);
            } else {
                writer->WriteStream(json_response);
            }
        }
    }
    writer->WriteDone();
}

void HttpApiServer::registerResponses() {
    http_server_.RegisterRoute("POST", "/inference",
            std::bind(inferResponse, _1, _2, engine_, params_, pipeline_));
    // TODO: register other routes
}

std::string Pipeline::decode(std::vector<int> token_ids) {
    py::gil_scoped_acquire acquire;
    std::string res = py::cast<std::string>(token_processor_.attr("decode")(token_ids));
    return res;
}

std::vector<int> Pipeline::encode(std::string prompt) {
    py::gil_scoped_acquire acquire;
    auto res = token_processor_.attr("encode")(prompt);
    std::vector<int> vecInt;
    if (!py::isinstance<py::list>(res)) {
        throw std::runtime_error("Expected a list, but get " + py::cast<std::string>(py::str(res)));
    }
    py::list py_list = py::reinterpret_borrow<py::list>(res);
    for (auto item : py_list) {
        vecInt.push_back(py::cast<int>(item));
    }
    return vecInt;
}

std::string Pipeline::format_response(std::string generate_texts,
                                      const GenerateOutputs* generate_outputs) {
    PipelineResponse res;
    res.response = generate_texts;
    res.finished = generate_outputs->generate_outputs[0].finished;
    res.aux_info = AuxInfoAdapter(generate_outputs->generate_outputs[0].aux_info);
    return ToJsonString(res, /*isCompact=*/true);
}

} // namespace rtp_llm
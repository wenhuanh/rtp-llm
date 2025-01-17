#include "hipblasAlgoMap.h"

#include "absl/container/inlined_vector.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_split.h"
#include "absl/strings/numbers.h"
#include "src/fastertransformer/utils/logger.h"

#include "rocm/include/hipblaslt/hipblaslt-ext.hpp"

#include <fstream>
#include <string>

namespace fastertransformer {
namespace rocm {

constexpr absl::string_view COLUMN_HEADER = "trans_a, trans_b, m, n, k, A_data_type, lda, stride_a, "
                                            "B_data_type, ldb, stride_b, C_data_type, ldc, stride_c, "
                                            "compute_type, batch_count, algo_index";

static absl::StatusOr<std::pair<hipblasLtAlgoConfig, int32_t>> parseRow(const std::string& row) {
    constexpr size_t                                  numFields = 17;
    absl::InlinedVector<absl::string_view, numFields> fields    = absl::StrSplit(row, ',');

    if (fields.size() != numFields) {
        return absl::InvalidArgumentError("Invalid number of fields in row: " + row);
    }

    hipblasLtAlgoConfig config;
    int32_t             algoIndex;

    auto parseOp = [](const absl::string_view& field) -> absl::StatusOr<hipblasOperation_t> {
        if (field == "T") {
            return HIPBLAS_OP_T;
        } else if (field == "N") {
            return HIPBLAS_OP_N;
        } else {
            return absl::InvalidArgumentError("Invalid hipblasOperation_t value: " + std::string(field));
        }
    };

    auto parseDType = [](const absl::string_view& field) -> absl::StatusOr<hipDataType> {
        if (field == "f32_r") {
            return HIP_R_32F;
        } else if (field == "f16_r") {
            return HIP_R_16F;
        } else if (field == "bf16_r") {
            return HIP_R_16BF;
        } else {
            return absl::InvalidArgumentError("Invalid hipDataType value: " + std::string(field));
        }
    };

    auto parseComputeType = [](const absl::string_view& field) -> absl::StatusOr<hipblasComputeType_t> {
        if (field == "f32_r") {
            return HIPBLAS_COMPUTE_32F;
        } else {
            return absl::InvalidArgumentError("Invalid hipblasComputeType_t value: " + std::string(field));
        }
    };

#define ASSIGN(lhs, rhs)                                                                                               \
    do {                                                                                                               \
        auto __tmp = (rhs);                                                                                            \
        if (!__tmp.ok())                                                                                               \
            return __tmp.status();                                                                                     \
        lhs = __tmp.value();                                                                                           \
    } while (0)

#define ATOI(field, member)                                                                                            \
    do {                                                                                                               \
        if (!absl::SimpleAtoi(field, &member))                                                                         \
            return absl::InvalidArgumentError("Invalid integer for " #member ": " + std::string(field));               \
    } while (0)

    ASSIGN(config.trans_a, parseOp(fields[0]));
    ASSIGN(config.trans_b, parseOp(fields[1]));
    ATOI(fields[2], config.m);
    ATOI(fields[3], config.n);
    ATOI(fields[4], config.k);
    ASSIGN(config.A_data_type, parseDType(fields[5]));
    ATOI(fields[6], config.lda);
    ATOI(fields[7], config.stride_a);
    ASSIGN(config.B_data_type, parseDType(fields[8]));
    ATOI(fields[9], config.ldb);
    ATOI(fields[10], config.stride_b);
    ASSIGN(config.C_data_type, parseDType(fields[11]));
    ATOI(fields[12], config.ldc);
    ATOI(fields[13], config.stride_c);
    ASSIGN(config.compute_type, parseComputeType(fields[14]));
    ATOI(fields[15], config.batch_count);
    ATOI(fields[16], algoIndex);

#undef ASSIGN
#undef ATOI

    return std::make_pair(config, algoIndex);
}

static const char* hipblasGetErrorEnum(hipblasStatus_t error)
{
    switch (error) {
        case HIPBLAS_STATUS_SUCCESS:
            return "HIPBLAS_STATUS_SUCCESS";

        case HIPBLAS_STATUS_NOT_INITIALIZED:
            return "HIPBLAS_STATUS_NOT_INITIALIZED";

        case HIPBLAS_STATUS_ALLOC_FAILED:
            return "HIPBLAS_STATUS_ALLOC_FAILED";

        case HIPBLAS_STATUS_INVALID_VALUE:
            return "HIPBLAS_STATUS_INVALID_VALUE";

        case HIPBLAS_STATUS_ARCH_MISMATCH:
            return "HIPBLAS_STATUS_ARCH_MISMATCH";

        case HIPBLAS_STATUS_MAPPING_ERROR:
            return "HIPBLAS_STATUS_MAPPING_ERROR";

        case HIPBLAS_STATUS_EXECUTION_FAILED:
            return "HIPBLAS_STATUS_EXECUTION_FAILED";

        case HIPBLAS_STATUS_INTERNAL_ERROR:
            return "HIPBLAS_STATUS_INTERNAL_ERROR";

        case HIPBLAS_STATUS_NOT_SUPPORTED:
            return "HIPBLAS_STATUS_NOT_SUPPORTED";

        case HIPBLAS_STATUS_UNKNOWN:
            return "HIPBLAS_STATUS_LICENSE_ERROR";

        case HIPBLAS_STATUS_HANDLE_IS_NULLPTR:
            return "HIPBLAS_STATUS_HANDLE_IS_NULLPTR";

        case HIPBLAS_STATUS_INVALID_ENUM:
            return "HIPBLAS_STATUS_INVALID_ENUM";
    }
    return "<unknown>";
} 
void hipblasAlgoMap::loadGemmConfig(const std::string& filename, hipblasLtHandle_t handle) {
    std::ifstream file;
    std::string   line;

    line.reserve(256);
    file.open(filename);

    if (!std::getline(file, line)) {
        std::printf("Gemm config not find: %s \n", filename.c_str());
        return ;
    }

    if (line != COLUMN_HEADER) {
        std::printf("MISMATCH %s | %s\n", line.c_str(), std::string(COLUMN_HEADER).c_str());
        abort();
    }

    std::vector<int>                              algoIndices{1};
    std::vector<hipblasLtMatmulHeuristicResult_t> algos;

    while (std::getline(file, line)) {

        if (line.c_str()[0] == '#')
            continue;

        auto config_and_algoIndex = parseRow(line);
        
        if (!config_and_algoIndex.ok()) {
            // std::printf("%s\n", config_and_algoIndex.status().ToString().c_str());
            continue;
        }

        hipblasLtMatmulDesc_t   opDesc;
        hipblasLtMatrixLayout_t ADesc, BDesc, CDesc;

        auto [config, algoIndex] = config_and_algoIndex.value();

        hipblasLtMatmulDescCreate(&opDesc, config.compute_type, HIP_R_32F);
        hipblasLtMatmulDescSetAttribute(opDesc, HIPBLASLT_MATMUL_DESC_TRANSA, &config.trans_a, sizeof(int32_t));
        hipblasLtMatmulDescSetAttribute(opDesc, HIPBLASLT_MATMUL_DESC_TRANSB, &config.trans_b, sizeof(int32_t));

        hipblasLtMatrixLayoutCreate(&ADesc,
                                    config.A_data_type,
                                    config.trans_a == HIPBLAS_OP_N ? config.m : config.k,
                                    config.trans_a == HIPBLAS_OP_N ? config.k : config.m,
                                    config.lda);
        hipblasLtMatrixLayoutCreate(&BDesc,
                                    config.B_data_type,
                                    config.trans_b == HIPBLAS_OP_N ? config.k : config.n,
                                    config.trans_b == HIPBLAS_OP_N ? config.n : config.k,
                                    config.ldb);
        hipblasLtMatrixLayoutCreate(&CDesc, config.C_data_type, config.m, config.n, config.ldc);

        algoIndices[0] = algoIndex;
        algos.clear();
        hipblasStatus_t sts = hipblaslt_ext::getAlgosFromIndex(handle, algoIndices, algos);
        // printf("status = %s, algoIndex = %d, line = %s\n", hipblasGetErrorEnum(sts), algoIndices[0], line.c_str());

        if(algos.size() > 0)
        {
            hipblasLtMatmulInfo i;
            i.algo = algos.at(0).algo;
            i.opDesc.reset(opDesc);
            i.ADesc.reset(ADesc);
            i.CDesc.reset(CDesc);
            i.BDesc.reset(BDesc);
            algo_map_.emplace(config, std::move(i));
        }
    }
}

const hipblasLtMatmulInfo* hipblasAlgoMap::getAlgo(const hipblasOperation_t   trans_a,
                                                   const hipblasOperation_t   trans_b,
                                                   const int32_t              m,
                                                   const int32_t              n,
                                                   const int32_t              k,
                                                   const hipDataType          A_data_type,
                                                   const int32_t              lda,
                                                   const int64_t              stride_a,
                                                   const hipDataType          B_data_type,
                                                   const int32_t              ldb,
                                                   const int64_t              stride_b,
                                                   const hipDataType          C_data_type,
                                                   const int32_t              ldc,
                                                   const int64_t              stride_c,
                                                   const hipblasComputeType_t compute_type,
                                                   const int32_t              batch_count) {
    hipblasLtAlgoConfig config{trans_a,
                               trans_b,
                               m,
                               n,
                               k,
                               A_data_type,
                               lda,
                               stride_a,
                               B_data_type,
                               ldb,
                               stride_b,
                               C_data_type,
                               ldc,
                               stride_c,
                               compute_type,
                               batch_count};

    auto iter = algo_map_.find(config);
    if (iter != algo_map_.end()) {
        return &iter->second;
    }

    auto opToString = [](hipblasOperation_t op) -> const char* {
        switch (op) {
            case HIPBLAS_OP_T:
                return "T";
            case HIPBLAS_OP_N:
                return "N";
            default:
                return "<?>";
        }
    };

    auto dataTypeToString = [](hipDataType t) -> const char* {
        switch (t) {
            case HIP_R_32F:
                return "f32_r";
            case HIP_R_16F:
                return "f16_r";
            case HIP_R_16BF:
                return "bf16_r";
            default:
                return "<?>";
        }
    };

    auto computeTypeToString = [](hipblasComputeType_t t) -> const char* {
        switch (t) {
            case HIPBLAS_COMPUTE_32F:
                return "f32_r";
            default:
                return "<?>";
        }
    };
    
    /*FT_LOG_WARNING("MISSING HIPBLASLT CONFIG:\n %s,%s,%d,%d,%d,%s,%d,%lld,%s,%d,%lld,%s,%d,%lld,%s,%d\n",
                   opToString(trans_a),
                   opToString(trans_b),
                   m,
                   n,
                   k,
                   dataTypeToString(A_data_type),
                   lda,
                   stride_a,
                   dataTypeToString(B_data_type),
                   ldb,
                   stride_b,
                   dataTypeToString(C_data_type),
                   ldc,
                   stride_c,
                   computeTypeToString(compute_type),
                   batch_count);*/

    return nullptr;
}

}  // namespace rocm
}  // namespace fastertransformer

#include <Engine/Jobs/JobExecutor.hpp>

EE_NAMESPACE_JOBS_BEGIN

// The one instantiation of the job-backed pipeline, compiled into the engine
// library and exported. Clients see `extern template` from JobExecutor.hpp and
// link against this copy, which keeps the template from being defined once per
// module (and keeps the binary smaller).
template class EE_API PipelineBase<JobExecutor>;

EE_NAMESPACE_JOBS_END

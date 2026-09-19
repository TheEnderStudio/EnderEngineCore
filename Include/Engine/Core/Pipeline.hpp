#pragma once

/**
 * @file Pipeline.hpp
 * @brief Convenience header for the JobSubsystem-backed pipeline.
 *
 * The pipeline machinery itself lives in PipelineBase.hpp, which knows nothing
 * about any particular execution engine. This header binds the two together:
 * it pulls in the Jobs-backed executor and gives the whole thing the short name
 * `Pipeline` inside the engine namespace.
 *
 * It is therefore the one Core header that reaches into the Jobs module. Code
 * that must stay independent of Jobs (or that wants a different executor) should
 * include PipelineBase.hpp directly:
 *
 * @code
 * #include <Engine/Core/PipelineBase.hpp>   // PipelineBase<SequentialExecutor>
 * #include <Engine/Jobs/JobExecutor.hpp>     // PipelineBase<JobExecutor>
 * @endcode
 */

#include <Engine/Core/PipelineBase.hpp>
#include <Engine/Jobs/JobExecutor.hpp>

EE_NAMESPACE_BEGIN

/// @brief The JobSubsystem-backed pipeline: PipelineBase<Jobs::JobExecutor>.
using Pipeline = Jobs::Pipeline;

/// @brief Task descriptor of the JobSubsystem-backed pipeline.
using PipelineTaskDesc = Pipeline::TaskDesc;

/// @brief Execution engine of the default pipeline.
using PipelineJobExecutor = Jobs::JobExecutor;

/// @brief A pipeline that runs entirely on the calling thread, with no Jobs
///        dependency at all. Useful for tests and headless builds.
using SequentialPipeline = PipelineBase<SequentialExecutor>;

EE_NAMESPACE_END

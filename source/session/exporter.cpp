#include <chrono>
#include <optional>
#include <thread>

#include "config.hpp"
#include "exporter.hpp"
#include "logging.hpp"

#ifdef WIN32
#define NOMINMAX
#include <Windows.h>
#endif

using namespace std::chrono_literals;
using namespace std::string_literals;
using json = nlohmann::json;

struct ExporterConfiguration
{
    int initialWorkers{ 1 };
    int maxWorkers{ -1 };     // use max according to hardware
};

void from_json(const json& j, ExporterConfiguration& c) {
    j.at("initialWorkers").get_to(c.initialWorkers);
    j.at("maxWorkers").get_to(c.maxWorkers);
}

ExporterJobEncoder::ExporterJobEncoder(std::atomic<bool>& error)
    : nEncodeJobs_(0), error_(error)
{
}

void ExporterJobEncoder::push(ExportJob job)
{
    std::lock_guard<std::mutex> guard(mutex_);
    queue_.push(std::move(job));
    nEncodeJobs_++;
}

ExportJob ExporterJobEncoder::encode()
{
    ExportJob job;

    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!queue_.empty()) {
            job = std::move(queue_.front());
            queue_.pop();
            nEncodeJobs_--;
        }
    }

    if (job)
    {
        FDN_DEBUG(job->name, " encoding");
        auto start = std::chrono::high_resolution_clock::now();

        job->encode();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration = (end - start);
        FDN_DEBUG(job->name, " encoding took ", duration.count(), "ms");
    }

    return job;
}

void ExporterJobEncoder::flush()
{
    while (!error_)
    {
        {
            std::lock_guard guard(mutex_);
            if (queue_.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(2ms);
    }
}

ExporterJobWriter::ExporterJobWriter(std::unique_ptr<MovieWriter> writer)
  : writer_(std::move(writer)),
    utilisation_(1.)
{
}

void ExporterJobWriter::writeAudioFrame(const uint8_t *data, size_t size, int64_t pts)
{
    writer_->writeAudioFrame(data, size, pts);
}

void ExporterJobWriter::enqueueWrite(std::future<ExportJob> encoded)
{
    std::lock_guard<std::mutex> guard(writeQueueMutex_);
    writeQueue_.push(std::move(encoded));
}

ExportJob ExporterJobWriter::write()
{
    std::lock_guard<std::mutex> guard(mutex_);
    std::optional<std::future<ExportJob>> earliestFutureWriteJob;

    // careful to only lock writeQueueMutex_ briefly, as the dispatcher needs to return
    // others can add to the queue while we hold mutex_, but only we have the earliest item
    // and will hang on to mutex_ until it is written
    {
        std::lock_guard<std::mutex> guard(writeQueueMutex_);
        if (!writeQueue_.empty()) {
            earliestFutureWriteJob = std::move(writeQueue_.front());
            writeQueue_.pop();
        } else
            return nullptr;
    }

    try {
        // wait for the value
        ExportJob job = earliestFutureWriteJob->get();

        FDN_DEBUG(job->name, " writing");

        // start idle timer first time we try to write to avoid false including setup time
        if (idleStart_ == std::chrono::high_resolution_clock::time_point())
            idleStart_ = std::chrono::high_resolution_clock::now();

        writeStart_ = std::chrono::high_resolution_clock::now();

        if (job->type() == ExportJobType::Video) {
            writer_->writeVideoFrame(&job->output.buffer[0], job->output.buffer.size());
        }
        else {
            writer_->writeAudioFrame(&job->output.buffer[0], job->output.buffer.size(), job->iFrameOrPts);
        }
        auto writeEnd = std::chrono::high_resolution_clock::now();

        // filtered update of utilisation_
        auto totalTime = (writeEnd - idleStart_);
        auto writeTime = (writeEnd - writeStart_);
        if (writeEnd != idleStart_)
        {
            const double alpha = 0.9;
            utilisation_ = (1.0 - alpha) * utilisation_ + alpha * ((double)writeTime.count() / totalTime.count());
        }
        idleStart_ = writeEnd;

        std::chrono::duration<double, std::milli> durationInMs = writeTime;
        FDN_DEBUG(job->name, " writing took ", durationInMs.count(), "ms  utilisation=", utilisation_);

        return job;
    }
    catch (...) {
        error_ = true;
        throw;
    }
}

void ExporterJobWriter::close()
{
    if (!error_)
    {
        writer_->flush();

        writer_->writeTrailer();
    }

    writer_->close();
}

ExporterWorker::ExporterWorker(
    std::atomic<bool>& error,
    ExporterJobEncoder& encoder, ExporterJobWriter& writer,
    const std::string& name)
    : quit_(false), error_(error),
      jobEncoder_(encoder), jobWriter_(writer),
      name_(name)
{
    worker_ = std::thread(&ExporterWorker::worker_start, this);
}

ExporterWorker::~ExporterWorker()
{
    quit_ = true;
    worker_.join();
}

#ifdef WIN32
//!!! hack - need to get this in a public header
extern "C" {
    extern LONG WINAPI FDNExpFilter(EXCEPTION_POINTERS* pExp, DWORD dwExpCode);
}
#endif


// static public interface for std::thread
void ExporterWorker::worker_start()
{
#ifdef WIN32
    __try {
#endif
        run();
#ifdef WIN32
    }
    __except (FDNExpFilter(GetExceptionInformation(), GetExceptionCode()))
    {
        error_ = true;
    }
#endif
}

// private
void ExporterWorker::run()
{
    FDN_NAME_THREAD(name_);
    FDN_DEBUG("starting worker");

    while (!quit_)
    {
        // if we hit an error, we shouldn't keep participating
        if (error_)
        {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        try {
            // we assume the exporter delivers frames in sequence; we can't
            // buffer unlimited frames for writing until we're give frame 1
            // at the end, for example.
            ExportJob job = jobEncoder_.encode();

            if (!job)
            {
                std::this_thread::sleep_for(2ms);
            }
            else
            {
                FDN_DEBUG("exporting ", job->name);

                // ownership of job is about to be passed onwards, so pull willEncode off it first
                auto willEncode = std::move(job->willEncode);
                willEncode.set_value(std::move(job));  //!!! this could be done by encode

                // dequeue any in-order writes
                // NOTE: other threads may be blocked here, even though they could get on with encoding
                //       this is ultimately not a problem as they'll be rate limited by how much can be
                //       written out - we don't want encoding to get too far ahead of writing, as that's
                //       a liveleak of the encode buffers
                //       each job that is written, we release one decode thread
                //       each job must do one write
                do
                {
                    ExportJob written = jobWriter_.write();
                    if (written)
                    {
                        break;
                    }

                    std::this_thread::sleep_for(1ms);
                } while (!error_);  // an error in a different thread will abort this thread's need to write
            }
        }
        catch (const std::exception& ex)
        {
            FDN_ERROR("error during export job: ", ex.what());
            //!!! should copy the exception and rethrow in main thread when it joins
            error_ = true;
        }
        catch (...)
        {
            FDN_ERROR("unknown error during export job");
            //!!! should copy the exception and rethrow in main thread when it joins
            error_ = true;
        }
    }
}



Exporter::Exporter(
    UniqueEncoder encoder,
    std::unique_ptr<MovieWriter> movieWriter)
  : encoder_(std::move(encoder)),
    videoJobFreeList_(std::function<std::unique_ptr<VideoExportJob>()>([&]() {
                        return std::make_unique<VideoExportJob>(encoder_->create());
                      })),
    audioJobFreeList_(std::function<std::unique_ptr<AudioExportJob>()>([&]() {
                        return std::make_unique<AudioExportJob>();
                      })),
    jobEncoder_(error_),
    jobWriter_(std::move(movieWriter))
{

    ExporterConfiguration config;
    try {
        fdn::config().at("exporter").get_to(config);
    }
    catch (...)
    {
    }

    if (config.maxWorkers == -1)
        concurrentThreadsSupported_ = std::thread::hardware_concurrency() + 1;  // we assume at least 1 thread will be blocked by io write
    else
        concurrentThreadsSupported_ = config.maxWorkers;

    // 1 thread to start with, super large textures can exhaust memory immediately with multiple jobs
    for (int i=0; i<config.initialWorkers; ++i)
        workers_.push_back(std::make_unique<ExporterWorker>(error_, jobEncoder_, jobWriter_,
                           "worker"s+std::to_string(workers_.size())));

    FDN_INFO("worker threads");
    FDN_INFO("  initial: ", workers_.size());
    FDN_INFO("  maximum: ", concurrentThreadsSupported_);
}

Exporter::~Exporter()
{
    try
    {
        close();
    }
    catch (...)
    {
        //!!! not much we can do now;
        //!!! users should call 'close' themselves if they need to catch errors
    }
}

void Exporter::close()
{
    if (!closed_)
    {
        FDN_INFO("closing export session - final worker pool size ", workers_.size());

        // we don't want to retry closing on destruction if we throw an exception
        closed_ = true;

        // wait for the job queue to empty
        jobEncoder_.flush();

        // wait for the workers to complete
        {
            ExportWorkers empty;  // this must be destructed before writer_.close()
            std::swap(workers_, empty);
        }

        // finalise and close the file.
        jobWriter_.close();

        if (error_)
            throw std::runtime_error("error writing");
    }
}


void Exporter::dispatchVideo(int64_t iFrame, const uint8_t* data, size_t stride, FrameFormat format) const
{
    // it is not clear from the docs whether or not frames are completed and passed in strict order
    // nevertheless we must make that assumption because
    //   -we don't know the start frame
    //   -we have finite space for buffering frames
    // so validate that here.
    // TODO: get confirmation from Adobe that this assumption cannot be violated
    if (!currentFrame_) {
        currentFrame_ = std::make_unique<int64_t>(iFrame);
    }
    else {
        // PremierePro can skip frames when there's no content
        // we will do the same - but we won't skip backwards
        if (*currentFrame_ >= iFrame)
        {
            // if this happens, this code needs revisiting
            throw std::runtime_error("plugin fault: frames delivered out of order");
        }
        ++(*currentFrame_);
    }

    auto startWait = std::chrono::high_resolution_clock::now();
    // throttle the caller - if the queue is getting too long we should wait
    while ((jobEncoder_.nEncodeJobs() >= std::max(size_t{ 1 }, workers_.size() - 1))  // first worker either encodes or writes
        && !expandWorkerPoolToCapacity()  // if we can, expand the pool
        && !error_)
    {
        // otherwise wait for an opening
        std::this_thread::sleep_for(2ms);
    }
    auto endWait = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> durationWaitInMs = endWait - startWait;

    // worker threads can die while encoding or writing (eg full disk)
    // this is the most likely spot where the error can be noted by the main thread
    // TODO: should intercept and alert with the correct error reason
    if (error_)
        throw std::runtime_error("error while exporting");

    ExportJob job = videoJobFreeList_.allocate();
    job->name = "video@"s + std::to_string(iFrame);
    FDN_DEBUG(job->name, " throttling took ", durationWaitInMs.count(), "ms");
    job->iFrameOrPts = iFrame;
    job->willEncode = std::promise<ExportJob>();

    // keep the writing order consistent with dispatches here
    jobWriter_.enqueueWrite(job->willEncode.get_future());

    // take a copy of the frame, in the codec preferred manner. we immediately return and let
    // the renderer get on with its job.
    //
    // TODO: may be able to use Adobe's addRef at a higher level and pipe it through for a minor
    //       performance gain

    auto start = std::chrono::high_resolution_clock::now();
    static_cast<VideoExportJob&>(*job).codecJob->copyExternalToLocal(data, stride, format);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> durationInMs = end - start;
    FDN_DEBUG(job->name, " queuing took ", durationInMs.count(), "ms");

    jobEncoder_.push(std::move(job));
}

void Exporter::dispatchAudio(int64_t pts, const uint8_t* data, size_t size) const
{
    // throttle the caller - if the queue is getting too long we should wait
    while ((jobEncoder_.nEncodeJobs() >= std::max(size_t{ 1 }, workers_.size() - 1))  // first worker either encodes or writes
        && !expandWorkerPoolToCapacity()  // if we can, expand the pool
        && !error_)
    {
        // otherwise wait for an opening
        std::this_thread::sleep_for(2ms);
    }

    // worker threads can die while encoding or writing (eg full disk)
    // this is the most likely spot where the error can be noted by the main thread
    // TODO: should intercept and alert with the correct error reason
    if (error_)
        throw std::runtime_error("error while exporting");

    ExportJob job = audioJobFreeList_.allocate();
    job->name = "audio@"s + std::to_string(pts);
    job->iFrameOrPts = pts;
    job->willEncode = std::promise<ExportJob>();

    // keep the writing order consistent with dispatches here
    jobWriter_.enqueueWrite(job->willEncode.get_future());

    // take a copy of the frame, in the codec preferred manner. we immediately return and let
    // the renderer get on with its job.
    job->output.buffer.resize(size);  // !!! would be nice to resize + copy in one step, avoiding zeroing
    std::copy(data, data + size, &job->output.buffer[0]);

    // !!! could go straight to the writer here
    jobEncoder_.push(std::move(job));
}

void Exporter::writeAudioFrame(const uint8_t *data, size_t size, int64_t pts)
{
    jobWriter_.writeAudioFrame(data, size, pts);
}

// returns true if pool was expanded
bool Exporter::expandWorkerPoolToCapacity() const
{
    bool isNotThreadLimited = workers_.size() < concurrentThreadsSupported_;
    bool isNotOutputLimited = jobWriter_.utilisation() < 0.99;
    bool isNotBufferLimited = true;  // TODO: get memoryUsed < maxMemoryCapacity from Adobe API

    if (isNotThreadLimited && isNotOutputLimited && isNotBufferLimited) {
        workers_.push_back(std::make_unique<ExporterWorker>(error_, jobEncoder_, jobWriter_,
                                                            "worker"s+std::to_string(workers_.size())));
        FDN_INFO("expanded worker pool to ", workers_.size());
        return true;
    }
    return false;
}

std::unique_ptr<Exporter> createExporter(
    const FrameSize& frameSize, CodecAlpha alpha, Codec4CC videoFormat, HapChunkCounts chunkCounts, int quality,
    Rational frameRate,
    int32_t maxFrames, int32_t reserveMetadataSpace,
    const MovieFile& file,
    std::optional<AudioDef> audio,
    bool writeMoovTagEarly
)
{
    std::unique_ptr<EncoderParametersBase> parameters = std::make_unique<EncoderParametersBase>(
        frameSize,
        alpha,
        videoFormat,
        chunkCounts,
        quality
        );

    UniqueEncoder encoder = CodecRegistry::codec()->createEncoder(std::move(parameters));

    // !!! instantiation of this class should be moved further out
    // !!! probably requires putting the encoder on it, which will simplify
    // !!! other things
    VideoDef video{
        frameSize.width,
        frameSize.height,
        videoFormat,
        CodecRegistry::codec()->details().fileFormatShortName,
        encoder->encodedBitDepth(),
        frameRate,
        maxFrames
    };

    std::unique_ptr<MovieWriter> writer = std::make_unique<MovieWriter>(
        reserveMetadataSpace,
        file,
        video,
        audio,
        writeMoovTagEarly   // writeMoovTagEarly
        );

    writer->writeHeader();

    return std::make_unique<Exporter>(std::move(encoder), std::move(writer));
}

#include <chrono>
#include <future>
#include <mutex>

#include <seqan3/io/sequence_file/input.hpp>
#include <seqan3/range/views/async_input_buffer.hpp>
#include <seqan3/range/views/chunk.hpp>
#include <seqan3/range/views/minimiser_hash.hpp>
#include <seqan3/range/views/zip.hpp>
#include <seqan3/search/dream_index/interleaved_bloom_filter.hpp>

#include <minimiser_model.hpp>
#include <shared.hpp>

using count_size_type = uint8_t;
inline constexpr bool use_simd = true;

template <typename ibf_t>
constexpr auto counting_agent(ibf_t && ibf) noexcept
{
    if constexpr (use_simd)
        return ibf.template simd_counting_agent<count_size_type>();
    else
        return ibf.template counting_agent<count_size_type>();
}

class sync_out
{
public:
    sync_out() = default;
    sync_out(sync_out const &) = default;
    sync_out & operator=(sync_out const &) = default;
    sync_out(sync_out &&) = default;
    sync_out & operator=(sync_out &&) = default;
    ~sync_out() = default;

    sync_out(std::filesystem::path const & path) : file(std::ofstream{path}) {}

    void write(std::string const & data)
    {
        std::lock_guard<std::mutex> lock(write_mutex);
        file << data;
    }

private:
    std::ofstream file;
    std::mutex write_mutex;
};

std::vector<size_t> compute_simple_model(search_arguments const & arguments)
{
    std::vector<size_t> precomp_thresholds;

    if (!arguments.threshold && !do_cerealisation_in(precomp_thresholds, arguments))
    {
        precomp_thresholds = precompute_threshold(arguments.pattern_size,
                                                  arguments.window_size,
                                                  arguments.kmer_size,
                                                  arguments.errors,
                                                  arguments.tau);

        do_cerealisation_out(precomp_thresholds, arguments);
    }

    return precomp_thresholds;
}

template <typename t>
void load_ibf(t & ibf, search_arguments const & arguments, size_t const part, double & ibf_io_time)
{
    std::filesystem::path ibf_file{arguments.ibf_file};
    ibf_file += "_" + std::to_string(part);
    std::ifstream is{ibf_file, std::ios::binary};
    cereal::BinaryInputArchive iarchive{is};
    auto start = std::chrono::high_resolution_clock::now();
    iarchive(ibf);
    auto end = std::chrono::high_resolution_clock::now();
    ibf_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
}

template <typename t>
inline void do_parallel(t && worker, size_t const num_records, size_t const threads, double & compute_time)
{
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<decltype(std::async(std::launch::async, worker, size_t{}, size_t{}))> tasks;
    size_t const records_per_thread = num_records / threads;

    for (size_t i = 0; i < threads; ++i)
    {
        size_t const start = records_per_thread * i;
        size_t const end = i == (threads-1) ? num_records: records_per_thread * (i+1);
        tasks.emplace_back(std::async(std::launch::async, worker, start, end));
    }

    for (auto && task : tasks)
        task.wait();

    auto end = std::chrono::high_resolution_clock::now();
    compute_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
}

template <bool compressed>
void run_program_multiple(search_arguments const & arguments)
{
    constexpr seqan3::data_layout ibf_data_layout = compressed ? seqan3::data_layout::compressed :
                                                                 seqan3::data_layout::uncompressed;
    auto ibf = seqan3::interleaved_bloom_filter<ibf_data_layout>{};

    seqan3::sequence_file_input<dna4_traits, seqan3::fields<seqan3::field::id, seqan3::field::seq>> fin{arguments.query_file};
    using record_type = typename decltype(fin)::record_type;
    std::vector<record_type> records{};

    double ibf_io_time{0.0};
    double reads_io_time{0.0};
    double compute_time{0.0};

    size_t const kmers_per_window = arguments.window_size - arguments.kmer_size + 1;
    size_t const kmers_per_pattern = arguments.pattern_size - arguments.kmer_size + 1;
    size_t const min_number_of_minimisers = kmers_per_window == 1 ? kmers_per_pattern :
                                                std::ceil(kmers_per_pattern / static_cast<double>(kmers_per_window));
    size_t const kmer_lemma = arguments.pattern_size + 1u > (arguments.errors + 1u) * arguments.kmer_size ?
                                arguments.pattern_size + 1u - (arguments.errors + 1u) * arguments.kmer_size :
                                0;
    size_t const max_number_of_minimisers = arguments.pattern_size - arguments.window_size + 1;
    std::vector<size_t> const precomp_thresholds = compute_simple_model(arguments);

    auto cereal_worker = [&] ()
    {
        load_ibf(ibf, arguments, 0, ibf_io_time);
    };

    for (auto && chunked_records : fin | seqan3::views::chunk((1ULL<<20)*10))
    {
        auto cereal_handle = std::async(std::launch::async, cereal_worker);

        records.clear();
        auto start = std::chrono::high_resolution_clock::now();
        std::ranges::move(chunked_records, std::cpp20::back_inserter(records));
        auto end = std::chrono::high_resolution_clock::now();
        reads_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

        std::vector<seqan3::counting_vector<count_size_type, use_simd>> counts(records.size(),
                                                                               seqan3::counting_vector<count_size_type, use_simd>(ibf.bin_count(), 0));

        auto count_task = [&](size_t const start, size_t const end)
        {
            auto counter = counting_agent(ibf);

            size_t counter_id = start;

            auto hash_view = seqan3::views::minimiser_hash(seqan3::ungapped{arguments.kmer_size},
                                                           seqan3::window_size{arguments.window_size},
                                                           seqan3::seed{adjust_seed(arguments.kmer_size)});

            for (auto && [id, seq] : records | seqan3::views::slice(start, end))
            {
                (void) id;
                auto & result = counter.bulk_count(seq | hash_view);
                counts[counter_id++] += result;
            }
        };

        cereal_handle.wait();
        do_parallel(count_task, records.size(), arguments.threads, compute_time);

        for (size_t const part : std::views::iota(1u, static_cast<unsigned int>(arguments.parts - 1)))
        {
            load_ibf(ibf, arguments, part, ibf_io_time);
            do_parallel(count_task, records.size(), arguments.threads, compute_time);
        }

        load_ibf(ibf, arguments, arguments.parts - 1, ibf_io_time);
        sync_out synced_out{arguments.out_file};

        auto output_task = [&](size_t const start, size_t const end)
        {
            auto counter = counting_agent(ibf);
            size_t counter_id = start;
            std::string result_string{};
            std::vector<uint64_t> minimiser;

            auto hash_view = seqan3::views::minimiser_hash(seqan3::ungapped{arguments.kmer_size},
                                                           seqan3::window_size{arguments.window_size},
                                                           seqan3::seed{adjust_seed(arguments.kmer_size)});

            for (auto && [id, seq] : records | seqan3::views::slice(start, end))
            {
                minimiser.clear();
                result_string.clear();
                result_string += id;
                result_string += '\t';

                minimiser = seq | hash_view | seqan3::views::to<std::vector<uint64_t>>;
                counts[counter_id] += counter.bulk_count(minimiser);
                size_t const minimiser_count{minimiser.size()};
                size_t current_bin{0};


                size_t const threshold = arguments.treshold_was_set ?
                                            static_cast<size_t>(minimiser_count * arguments.threshold) :
                                            kmers_per_window == 1 ? kmer_lemma :
                                            precomp_thresholds[std::min(minimiser_count < min_number_of_minimisers ?
                                                                            0 :
                                                                            minimiser_count - min_number_of_minimisers,
                                                                        max_number_of_minimisers -
                                                                            min_number_of_minimisers)] + 2;

                for (auto && count : counts[counter_id++])
                {
                    if (count >= threshold)
                    {
                        result_string += std::to_string(current_bin);
                        result_string += ',';
                    }
                    ++current_bin;
                }
                result_string += '\n';
                synced_out.write(result_string);
            }
        };

        do_parallel(output_task, records.size(), arguments.threads, compute_time);
    }

    if (arguments.write_time)
    {
        std::filesystem::path file_path{arguments.out_file};
        file_path += ".time";
        std::ofstream file_handle{file_path};
        file_handle << "IBF I/O\tReads I/O\tCompute\n";
        file_handle << std::fixed
                    << std::setprecision(2)
                    << ibf_io_time << '\t'
                    << reads_io_time << '\t'
                    << compute_time;
    }
}

template <bool compressed>
void run_program_single(search_arguments const & arguments)
{
    constexpr seqan3::data_layout ibf_data_layout = compressed ? seqan3::data_layout::compressed :
                                                                 seqan3::data_layout::uncompressed;
    auto ibf = seqan3::interleaved_bloom_filter<ibf_data_layout>{};

    std::ifstream is{arguments.ibf_file, std::ios::binary};
    cereal::BinaryInputArchive iarchive{is};

    double ibf_io_time{0.0};
    double reads_io_time{0.0};
    double compute_time{0.0};

    auto cereal_worker = [&] ()
    {
        auto start = std::chrono::high_resolution_clock::now();
        iarchive(ibf);
        auto end = std::chrono::high_resolution_clock::now();
        ibf_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
    };
    auto cereal_handle = std::async(std::launch::async, cereal_worker);

    seqan3::sequence_file_input<dna4_traits, seqan3::fields<seqan3::field::id, seqan3::field::seq>> fin{arguments.query_file};
    using record_type = typename decltype(fin)::record_type;
    std::vector<record_type> records{};

    sync_out synced_out{arguments.out_file};

    size_t const kmers_per_window = arguments.window_size - arguments.kmer_size + 1;
    size_t const kmers_per_pattern = arguments.pattern_size - arguments.kmer_size + 1;
    size_t const min_number_of_minimisers = kmers_per_window == 1 ? kmers_per_pattern :
                                                std::ceil(kmers_per_pattern / static_cast<double>(kmers_per_window));
    size_t const kmer_lemma = arguments.pattern_size + 1u > (arguments.errors + 1u) * arguments.kmer_size ?
                                arguments.pattern_size + 1u - (arguments.errors + 1u) * arguments.kmer_size :
                                0;
    size_t const max_number_of_minimisers = arguments.pattern_size - arguments.window_size + 1;
    std::vector<size_t> const precomp_thresholds = compute_simple_model(arguments);

    auto worker = [&] (size_t const start, size_t const end)
    {
        auto counter = counting_agent(ibf);
        std::string result_string{};
        std::vector<uint64_t> minimiser;

        auto hash_view = seqan3::views::minimiser_hash(seqan3::ungapped{arguments.kmer_size},
                                                       seqan3::window_size{arguments.window_size},
                                                       seqan3::seed{adjust_seed(arguments.kmer_size)});

        for (auto && [id, seq] : records | seqan3::views::slice(start, end))
        {
            minimiser.clear();
            result_string.clear();
            result_string += id;
            result_string += '\t';

            minimiser = seq | hash_view | seqan3::views::to<std::vector<uint64_t>>;
            auto & result = counter.bulk_count(minimiser);
            size_t const minimiser_count{minimiser.size()};
            size_t current_bin{0};

            size_t const threshold = arguments.treshold_was_set ?
                                         static_cast<size_t>(minimiser_count * arguments.threshold) :
                                         kmers_per_window == 1 ? kmer_lemma :
                                         precomp_thresholds[std::min(minimiser_count < min_number_of_minimisers ?
                                                                         0 :
                                                                         minimiser_count - min_number_of_minimisers,
                                                                     max_number_of_minimisers -
                                                                         min_number_of_minimisers)] + 2;

            for (auto && count : result)
            {
                if (count >= threshold)
                {
                    result_string += std::to_string(current_bin);
                    result_string += ',';
                }
                ++current_bin;
            }
            result_string += '\n';
            synced_out.write(result_string);
        }
    };

    for (auto && chunked_records : fin | seqan3::views::chunk((1ULL<<20)*10))
    {
        records.clear();
        auto start = std::chrono::high_resolution_clock::now();
        std::ranges::move(chunked_records, std::cpp20::back_inserter(records));
        auto end = std::chrono::high_resolution_clock::now();
        reads_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

        cereal_handle.wait();

        do_parallel(worker, records.size(), arguments.threads, compute_time);
    }

    if (arguments.write_time)
    {
        std::filesystem::path file_path{arguments.out_file};
        file_path += ".time";
        std::ofstream file_handle{file_path};
        file_handle << "IBF I/O\tReads I/O\tCompute\n";
        file_handle << std::fixed
                    << std::setprecision(2)
                    << ibf_io_time << '\t'
                    << reads_io_time << '\t'
                    << compute_time;
    }
}

void raptor_search(search_arguments const & arguments)
{
    if (arguments.parts == 1)
    {
        if (arguments.compressed)
            run_program_single<true>(arguments);
        else
            run_program_single<false>(arguments);
    }
    else
    {
        if (arguments.compressed)
            run_program_multiple<true>(arguments);
        else
            run_program_multiple<false>(arguments);
    }

    return;
}

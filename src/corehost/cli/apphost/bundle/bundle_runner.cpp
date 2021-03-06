// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "bundle_runner.h"
#include "pal.h"
#include "trace.h"
#include "utils.h"

using namespace bundle;

void bundle_runner_t::seek(FILE* stream, long offset, int origin)
{
    if (fseek(stream, offset, origin) != 0)
    {
        trace::error(_X("Failure processing application bundle; possible file corruption."));
        trace::error(_X("I/O seek failure within the bundle."));
        throw StatusCode::BundleExtractionIOError;
    }
}

void bundle_runner_t::write(const void* buf, size_t size, FILE *stream)
{
    if (fwrite(buf, 1, size, stream) != size)
    {
        trace::error(_X("Failure extracting contents of the application bundle."));
        trace::error(_X("I/O failure when writing extracted files."));
        throw StatusCode::BundleExtractionIOError;
    }
}

void bundle_runner_t::read(void* buf, size_t size, FILE* stream)
{
    if (fread(buf, 1, size, stream) != size)
    {
        trace::error(_X("Failure processing application bundle; possible file corruption."));
        trace::error(_X("I/O failure reading contents of the bundle."));
        throw StatusCode::BundleExtractionIOError;
    }
}

// Handle the relatively uncommon scenario where the bundle ID or 
// the relative-path of a file within the bundle is longer than 127 bytes
size_t bundle_runner_t::get_path_length(int8_t first_byte, FILE* stream)
{
    size_t length = 0;

    // If the high bit is set, it means there are more bytes to read.
    if ((first_byte & 0x80) == 0)
    {
         length = first_byte;
    }
    else
    {
        int8_t second_byte = 0;
        read(&second_byte, 1, stream);

        if (second_byte & 0x80)
        {
            // There can be no more than two bytes in path_length
            trace::error(_X("Failure processing application bundle; possible file corruption."));
            trace::error(_X("Path length encoding read beyond two bytes"));

            throw StatusCode::BundleExtractionFailure;
        }

        length = (second_byte << 7) | (first_byte & 0x7f);
    }

    if (length <= 0 || length > PATH_MAX)
    {
        trace::error(_X("Failure processing application bundle; possible file corruption."));
        trace::error(_X("Path length is zero or too long"));
        throw StatusCode::BundleExtractionFailure;
    }

    return length;
}

// Read a non-null terminated fixed length UTF8 string from a byte-stream
// and transform it to pal::string_t
void bundle_runner_t::read_string(pal::string_t &str, size_t size, FILE* stream)
{
    std::unique_ptr<uint8_t[]> buffer{new uint8_t[size + 1]};
    read(buffer.get(), size, stream);
    buffer[size] = 0; // null-terminator
    pal::clr_palstring(reinterpret_cast<const char*>(buffer.get()), &str);
}

static bool has_dirs_in_path(const pal::string_t& path)
{
    return path.find_last_of(DIR_SEPARATOR) != pal::string_t::npos;
}

static void create_directory_tree(const pal::string_t &path)
{
    if (path.empty())
    {
        return;
    }

    if (pal::directory_exists(path))
    {
        return;
    }

    if (has_dirs_in_path(path))
    {
        create_directory_tree(get_directory(path));
    }

    if (!pal::mkdir(path.c_str(), 0700)) // Owner - rwx
    {
        if (pal::directory_exists(path))
        {
            // The directory was created since we last checked.
            return;
        }

        trace::error(_X("Failure processing application bundle."));
        trace::error(_X("Failed to create directory [%s] for extracting bundled files"), path.c_str());
        throw StatusCode::BundleExtractionIOError;
    }
}

static void remove_directory_tree(const pal::string_t& path)
{
    if (path.empty())
    {
        return;
    }

    std::vector<pal::string_t> dirs;
    pal::readdir_onlydirectories(path, &dirs);

    for (const pal::string_t &dir : dirs)
    {
        pal::string_t dir_path = path;
        append_path(&dir_path, dir.c_str());

        remove_directory_tree(dir_path);
    }

    std::vector<pal::string_t> files;
    pal::readdir(path, &files);

    for (const pal::string_t &file : files)
    {
        pal::string_t file_path = path;
        append_path(&file_path, file.c_str());

        if (!pal::remove(file_path.c_str()))
        {
            trace::warning(_X("Failed to remove temporary file [%s]."), file_path.c_str());
        }
    }

    if (!pal::rmdir(path.c_str()))
    {
        trace::warning(_X("Failed to remove temporary directory [%s]."), path.c_str());
    }
}

// Retry the rename operation with some wait in between the attempts.
// This is an attempt to workaround for possible file locking caused by AV software.

bool rename_with_retries(pal::string_t& old_name, pal::string_t& new_name, bool& file_exists)
{
    for (int retry_count = 0; retry_count < 500; retry_count++)
    {
        if (pal::rename(old_name.c_str(), new_name.c_str()) == 0)
        {
            return true;
        }
        bool should_retry = errno == EACCES;

        if (pal::file_exists(new_name))
        {
            // Check file_exists() on each run, because a concurrent process may have
            // created the new_name file/directory.
            //
            // The rename() operation above fails with errono == EACCESS if 
            // * Directory new_name already exists, or
            // * Paths are invalid paths, or
            // * Due to locking/permission problems.
            // Therefore, we need to perform the directory_exists() check again.

            file_exists = true;
            return false;
        }

        if (should_retry)
        {
            trace::info(_X("Retrying Rename [%s] to [%s] due to EACCES error"), old_name.c_str(), new_name.c_str());
            pal::sleep(100);
            continue;
        }
        else
        {
            return false;
        }
    }

    return false;
}

void bundle_runner_t::reopen_host_for_reading()
{
    m_bundle_stream = pal::file_open(m_bundle_path, _X("rb"));
    if (m_bundle_stream == nullptr)
    {
        trace::error(_X("Failure processing application bundle."));
        trace::error(_X("Couldn't open host binary for reading contents"));
        throw StatusCode::BundleExtractionIOError;
    }
}

// Compute the final extraction location as:
// m_extraction_dir = $DOTNET_BUNDLE_EXTRACT_BASE_DIR/<app>/<id>/...
//
// If DOTNET_BUNDLE_EXTRACT_BASE_DIR is not set in the environment, 
// a default is choosen within the temporary directory.
void bundle_runner_t::determine_extraction_dir()
{
    if (!pal::getenv(_X("DOTNET_BUNDLE_EXTRACT_BASE_DIR"), &m_extraction_dir))
    {
        if (!pal::get_default_bundle_extraction_base_dir(m_extraction_dir))
        {
            trace::error(_X("Failure processing application bundle."));
            trace::error(_X("Failed to determine location for extracting embedded files"));
            trace::error(_X("DOTNET_BUNDLE_EXTRACT_BASE_DIR is not set, and a read-write cache directory couldn't be created."));

            throw StatusCode::BundleExtractionFailure;
        }
    }

    pal::string_t host_name = strip_executable_ext(get_filename(m_bundle_path));
    append_path(&m_extraction_dir, host_name.c_str());
    append_path(&m_extraction_dir, bundle_id().c_str());

    trace::info(_X("Files embedded within the bundled will be extracted to [%s] directory"), m_extraction_dir.c_str());
}

// Compute the worker extraction location for this process, before the 
// extracted files are committed to the final location
// m_working_extraction_dir = $DOTNET_BUNDLE_EXTRACT_BASE_DIR/<app>/<proc-id-hex>
void bundle_runner_t::create_working_extraction_dir()
{
    // Set the working extraction path
    m_working_extraction_dir = get_directory(m_extraction_dir);
    pal::char_t pid[32];
    pal::snwprintf(pid, 32, _X("%x"), pal::get_pid());
    append_path(&m_working_extraction_dir, pid);

    create_directory_tree(m_working_extraction_dir);

    trace::info(_X("Temporary directory used to extract bundled files is [%s]"), m_working_extraction_dir.c_str());
}

// Create a file to be extracted out on disk, including any intermediate sub-directories.
FILE* bundle_runner_t::create_extraction_file(const pal::string_t& relative_path)
{
    pal::string_t file_path = m_working_extraction_dir;
    append_path(&file_path, relative_path.c_str());

    // m_working_extraction_dir is assumed to exist, 
    // so we only create sub-directories if relative_path contains directories
    if (has_dirs_in_path(relative_path))
    {
        create_directory_tree(get_directory(file_path));
    }

    FILE* file = pal::file_open(file_path.c_str(), _X("wb"));

    if (file == nullptr)
    {
        trace::error(_X("Failure processing application bundle."));
        trace::error(_X("Failed to open file [%s] for writing"), file_path.c_str());
        throw StatusCode::BundleExtractionIOError;
    }

    return file;
}

// Extract one file from the bundle to disk.
void bundle_runner_t::extract_file(file_entry_t *entry)
{
    FILE* file = create_extraction_file(entry->relative_path());
    const int64_t buffer_size = 8 * 1024; // Copy the file in 8KB chunks
    uint8_t buffer[buffer_size];
    int64_t file_size = entry->size();

    seek(m_bundle_stream, entry->offset(), SEEK_SET);
    do {
        int64_t copy_size = (file_size <= buffer_size) ? file_size : buffer_size;
        read(buffer, copy_size, m_bundle_stream);
        write(buffer, copy_size, file);
        file_size -= copy_size;
    } while (file_size > 0);

    fclose(file);
}

void bundle_runner_t::commit_file(const pal::string_t& relative_path)
{
    // Commit individual files to the final extraction directory.

    pal::string_t working_file_path = m_working_extraction_dir;
    append_path(&working_file_path, relative_path.c_str());

    pal::string_t final_file_path = m_extraction_dir;
    append_path(&final_file_path, relative_path.c_str());

    if (has_dirs_in_path(relative_path))
    {
        create_directory_tree(get_directory(final_file_path));
    }

    bool extracted_by_concurrent_process = false;
    bool extracted_by_current_process =
        rename_with_retries(working_file_path, final_file_path, extracted_by_concurrent_process);

    if (extracted_by_concurrent_process)
    {
        // Another process successfully extracted the dependencies
        trace::info(_X("Extraction completed by another process, aborting current extraction."));
    }

    if (!extracted_by_current_process && !extracted_by_concurrent_process)
    {
        trace::error(_X("Failure processing application bundle."));
        trace::error(_X("Failed to commit extracted files to directory [%s]."), m_extraction_dir.c_str());
        throw StatusCode::BundleExtractionFailure;
    }

    trace::info(_X("Extraction recovered [%s]"), relative_path.c_str());
}

void bundle_runner_t::commit_dir()
{
    // Commit an entire new extraction to the final extraction directory
    // Retry the move operation with some wait in between the attempts. This is to workaround for possible file locking
    // caused by AV software. Basically the extraction process above writes a bunch of executable files to disk
    // and some AV software may decide to scan them on write. If this happens the files will be locked which blocks
    // our ablity to move them.

    bool extracted_by_concurrent_process = false;
    bool extracted_by_current_process =
        rename_with_retries(m_working_extraction_dir, m_extraction_dir, extracted_by_concurrent_process);

    if (extracted_by_concurrent_process)
    {
        // Another process successfully extracted the dependencies
        trace::info(_X("Extraction completed by another process, aborting current extraction."));
        remove_directory_tree(m_working_extraction_dir);
    }

    if (!extracted_by_current_process && !extracted_by_concurrent_process)
    {
        trace::error(_X("Failure processing application bundle."));
        trace::error(_X("Failed to commit extracted files to directory [%s]."), m_extraction_dir.c_str());
        throw StatusCode::BundleExtractionFailure;
    }

    trace::info(_X("Completed new extraction."));
}

// Verify that an existing extraction contains all files listed in the bundle manifest.
// If some files are missing, extract them individually.
void bundle_runner_t::verify_recover_extraction()
{
    bool recovered = false;

    for (file_entry_t* entry : m_manifest->files)
    {
        pal::string_t file_path = m_extraction_dir;
        append_path(&file_path, entry->relative_path().c_str());

        if (!pal::file_exists(file_path))
        {
            if (!recovered)
            {
                recovered = true;
                create_working_extraction_dir();
            }

            extract_file(entry);
            commit_file(entry->relative_path());
        }
    }

    if (recovered)
    {
        remove_directory_tree(m_working_extraction_dir);
    }
}

// Current support for executing single-file bundles involves 
// extraction of embedded files to actual files on disk. 
// This method implements the file extraction functionality at startup.
StatusCode bundle_runner_t::extract()
{
    try
    {
        reopen_host_for_reading();

        // Read the bundle header
        seek(m_bundle_stream, marker_t::header_offset(), SEEK_SET);
        m_header.reset(header_t::read(m_bundle_stream));

        m_manifest.reset(manifest_t::read(m_bundle_stream, num_embedded_files()));

        // Determine if embedded files are already extracted, and available for reuse
        determine_extraction_dir();
        if (pal::directory_exists(m_extraction_dir))
        {
            // If so, verify the contents, recover files if necessary, 
            // and use the existing extraction.

            verify_recover_extraction();
            fclose(m_bundle_stream);
            return StatusCode::Success;
        }

        // If not, create a new extraction

        // Extract files to temporary working directory
        //
        // Files are extracted to a specific deterministic location on disk
        // on first run, and are available for reuse by subsequent similar runs.
        //
        // The extraction should be fault tolerant with respect to:
        //  * Failures/crashes during extraction which result in partial-extraction
        //  * Race between two or more processes concurrently attempting extraction
        //
        // In order to solve these issues, we implement a extraction as a two-phase approach:
        // 1) Files embedded in a bundle are extracted to a process-specific temporary
        //    extraction location (m_working_extraction_dir)
        // 2) Upon successful extraction, m_working_extraction_dir is renamed to the actual
        //    extraction location (m_extraction_dir)
        //    
        // This effectively creates a file-lock to protect against races and failed extractions.
        
        create_working_extraction_dir();

        for (file_entry_t* entry : m_manifest->files) {
            extract_file(entry);
        }

        commit_dir();

        fclose(m_bundle_stream);
        return StatusCode::Success;
    }
    catch (StatusCode e)
    {
        fclose(m_bundle_stream);
        return e;
    }
}

#include <vector>
#include <iostream>
#include <fstream>
#include <iterator>
#include <chrono>
#include <cmath>
#include <numeric>
#include <cassert>
#include <omp.h>
#include <mpi.h>
#include <argparse.hpp>

// Get the number of processes
int mpi_size;

// Get the rank of the process
int mpi_rank;

// The number of projections and the detector dimensions
constexpr int num_projections = 320;
constexpr int detector_rows = 192;
constexpr int detector_columns = 256;


// TODO: this function should use MPI-IO to read files
// TODO: use either file pointer or explicit offsets

/** Read binary file
 *
 * @param size      The number of elements to read (in floats)
 * @param offset    The offset into the file to read (in floats)
 * @param filename  The filename
 * @return          The data read
 */
std::vector<float> read_file(uint64_t size, uint64_t offset, const std::string &filename) {
    std::vector<float> data(size);
    std::ifstream file(filename, std::ios::in | std::ifstream::binary);
    if (not file.good()) {
        throw std::runtime_error("Couldn't read to file: " + filename);
    }
    file.seekg(offset * sizeof(float), std::ios::beg);
    file.read(reinterpret_cast<char *>(&data[0]), data.size() * sizeof(float));
    return data;
}

/** Write binary file
 *
 * @param data       The data to write
 * @param offset     The offset into the file to write (in floats)
 * @param filename   The filename
 */
void write_file(std::vector<float> data, uint64_t offset, const std::string &filename) {
    std::ofstream file(filename, std::ios::out | std::ifstream::binary);
    if (not file.good()) {
        throw std::runtime_error("Couldn't write to file: " + filename);
    }
    file.seekp(offset * sizeof(float), std::ios::beg);
    file.write(reinterpret_cast<char *>(&data[0]), data.size() * sizeof(float));
}

/** CT data that are used for all projections */
class GlobalData {
public:
    // Matrix with all combinations of all X,Y coordinates for the 3D volume.
    std::vector<float> combined_matrix;
    // Matrix with Z coordinates for the 3D volume.
    std::vector<float> z_voxel_coords;
};

/** CT data that are associated with a specific projection */
class ProjectionData {
public:
    // Matrix with the pre-processed 2D X-Ray image
    std::vector<float> projection;
    // Matrix used to map 3D coordinates to 2D coordinates.
    std::vector<float> transform_matrix;
    // Post weight to compensate for the cone effect of the X-Ray beam.
    std::vector<float> volume_weight;
};

/** Load global CT data
 *
 * @param num_voxels  Number of voxels (assumed cubed)
 * @param input_dir   The CT data directory
 * @return            The global CT data
 */
GlobalData load_global_data(int num_voxels, const std::string &input_dir) {
    std::string voxel_dir = input_dir + "/" + std::to_string(num_voxels);
    GlobalData data;

    // Load combined X,Y voxel coordinates
    data.combined_matrix = read_file(4 * num_voxels * num_voxels, 0, voxel_dir + "/combined.bin");

    // Load Z voxel coordinates
    data.z_voxel_coords = read_file(num_voxels, 0, voxel_dir + "/z_voxel_coords.bin");

    return data;
}

/**  Load projection specific CT data
 *
 * @param projection_id  The id of the projection to load
 * @param num_voxels     Number of voxels (assumed cubed)
 * @param input_dir      The CT data directory
 * @return               The projection specific CT data
 */
ProjectionData load_projection_data(int projection_id, int num_voxels, const std::string &input_dir) {
    std::string voxel_dir = input_dir + "/" + std::to_string(num_voxels);
    ProjectionData data;

    // Load 2D projection data
    data.projection = read_file(detector_rows * detector_columns, projection_id * detector_rows * detector_columns,
                                input_dir + "/projections.bin");

    // Load transform matrix used to align the 3D volume position towards the recorded 2D projection
    data.transform_matrix = read_file(3 * 4, projection_id * 3 * 4, input_dir + "/transform.bin");

    // Load volume weight used to compensate for cone beam ray density
    data.volume_weight = read_file(num_voxels * num_voxels, projection_id * num_voxels * num_voxels,
                                   voxel_dir + "/volumeweight.bin");
    return data;
}

/** Perform the CT reconstruction
 *
 * @param num_voxels       Number of voxels (assumed cubed)
 * @param input_dir        The CT data directory
 * @param output_filename  The name of the output file
 */
void reconstruction(int num_voxels, const std::string &input_dir, const std::string &output_filename) {

    // TODO: time read of projection data, compute and write of results indepedently
    // Notice, in this assignment we also time the disk access
    auto begin = std::chrono::steady_clock::now();

    double checksum = 0;
    GlobalData gdata = load_global_data(num_voxels, input_dir);
    // The size of the reconstruction volume is assumed a cube.
    uint64_t recon_volume_size = num_voxels * num_voxels * num_voxels;
    std::vector<float> recon_volume(recon_volume_size, 0);

    // TODO: change to only loop over the projections relevant for the MPI rank
    for (int projection_id = 0; projection_id < num_projections; ++projection_id) {
        ProjectionData pdata = load_projection_data(projection_id, num_voxels, input_dir);

        // TODO: Use OpenMP to parallelise local calculation
        for (int z = 0; z < num_voxels; ++z) {
            uint64_t size = num_voxels * num_voxels;
            for (uint64_t i = 0; i < size; ++i) {
                // Find the mapping between volume voxels and detector pixels for the current projection angle
                float vol_det_map_0 = 0, vol_det_map_1 = 0, vol_det_map_2 = 0;
                for (uint64_t j = 0; j < 4; ++j) {
                    float combined_val = (j == 2) ? gdata.z_voxel_coords[z] : gdata.combined_matrix[j * size + i];
                    vol_det_map_0 += combined_val * pdata.transform_matrix[j];
                    vol_det_map_1 += combined_val * pdata.transform_matrix[j + 4];
                    vol_det_map_2 += combined_val * pdata.transform_matrix[j + 4 + 4];
                }
                int32_t map_col = std::round(vol_det_map_0 / vol_det_map_2);
                int32_t map_row = std::round(vol_det_map_1 / vol_det_map_2);

                // Find the detector pixels that contribute to the current slice
                // x-rays that hit outside the detector area are masked out
                if (map_col >= 0 && map_row >= 0 && map_col < detector_columns && map_row < detector_rows) {
                    // Add the weighted projection pixel values to their corresponding voxels in the z slice
                    recon_volume[z * size + i] +=
                            pdata.projection[map_col + map_row * detector_columns] * pdata.volume_weight[i];
                }
            }
        }
    }

    // TODO: gather `recon_volume_size` from all the MPI-processes and combine (sum) them into the final reconstruction
    MPI_Barrier(MPI_COMM_WORLD);
    if (mpi_rank == 0) {
        if (!output_filename.empty()) {
            write_file(recon_volume, 0, output_filename);
        }
        checksum += std::accumulate(recon_volume.begin(), recon_volume.end(), 0.0);
        auto end = std::chrono::steady_clock::now();
        std::cout << "checksum: " << checksum << std::endl;
        std::cout << "elapsed time: " << (end - begin).count() / 1000000000.0 << " sec" << std::endl;
    }
}


/** Main function that parses the command line and start the simulation */
int main(int argc, char **argv) {
    // Initialize the MPI environment
    MPI_Init(&argc, &argv);

    util::ArgParser args(argc, argv);
    int64_t num_voxels;
    std::string input_dir;
    if (args.cmdOptionExists("--num-voxels")) {
        num_voxels = std::stoi(args.getCmdOption("--num-voxels"));
    } else {
        throw std::invalid_argument("You must specify the number of voxels (e.g. --num-voxels 128)");
    }
    if (args.cmdOptionExists("--input")) {
        input_dir = args.getCmdOption("--input");
    } else {
        throw std::invalid_argument("You must specify the input directory (e.g. --input ./input");
    }
    const std::string &output_filename = args.getCmdOption("--out");

    // Get the number of processes
    MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);

    // Get the rank of the process
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);

    // Get the name of the processor
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len;
    MPI_Get_processor_name(processor_name, &name_len);

    // Print off a hello world message
    printf("CT Reconstruction running on `%s`, rank %d out of %d.\n", processor_name, mpi_rank, mpi_size);

    reconstruction(num_voxels, input_dir, output_filename);

    MPI_Finalize();
    return 0;
}

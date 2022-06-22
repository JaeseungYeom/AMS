#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "mfem.hpp"
#include "mfem/general/forall.hpp"
#include "mfem/linalg/dtensor.hpp"
#include "umpire/ResourceManager.hpp"

using mfem::ForallWrap;

#include "app/eos.hpp"
#include "ml/hdcache.hpp"
#include "ml/surrogate.hpp"
#include "utils/data_handler.hpp"
#include "utils/mfem_utils.hpp"
#include "wf/basedb.hpp"
#include "wf/utilities.hpp"

// This is usefull to completely remove
// caliper at compile time.
#ifdef __ENABLE_CALIPER__
#include <caliper/cali-manager.h>
#include <caliper/cali.h>
#define CALIPER(stmt) stmt
#else
#define CALIPER(stmt)
#endif

#define NEW_PACKING

//! ----------------------------------------------------------------------------
//! mini app class
//! ----------------------------------------------------------------------------
class MiniApp {
    using TypeValue = double;
    using data_handler = DataHandler<TypeValue>;

   public:
    bool is_cpu = true;
    bool pack_sparse_mats = true;
    int num_mats = 5;
    int num_elems = 10000;
    int num_qpts = 64;
    CALIPER(cali::ConfigManager mgr;)

    std::vector<EOS*> eoses;

    // added to include ML
    std::vector<HDCache<TypeValue>*> hdcaches;
    std::vector<SurrogateModel<TypeValue>*> surrogates;

    // Added to include an offline DB
    // (currently implemented as a file)
    BaseDB* DB = nullptr;

    // -------------------------------------------------------------------------
    // constructor and destructor
    // -------------------------------------------------------------------------
    MiniApp(int _num_mats, int _num_elems, int _num_qpts, bool _is_cpu, bool _pack_sparse_mats) {
        is_cpu = _is_cpu;
        num_mats = _num_mats;
        num_elems = _num_elems;
        num_qpts = _num_qpts;
        pack_sparse_mats = _pack_sparse_mats;

#ifdef __ENABLE_DB__
        DB = new BaseDB("miniApp_data.txt");
        if (!DB) {
            std::cout << "Cannot create static database\n";
        }
#endif

        // setup eos
        eoses.resize(num_mats, nullptr);
        hdcaches.resize(num_mats, nullptr);
        surrogates.resize(num_mats, nullptr);
    }

    void start() { CALIPER(mgr.start();) }

    ~MiniApp() {
        CALIPER(mgr.flush());
        for (int mat_idx = 0; mat_idx < num_mats; ++mat_idx) {
            delete eoses[mat_idx];
            delete hdcaches[mat_idx];
            delete surrogates[mat_idx];
        }
        delete DB;
    }

    // -------------------------------------------------------------------------
    // the main loop
    // -------------------------------------------------------------------------
    void evaluate_inner(const int mat_idx, const int num_data, double* pDensity, double* pEnergy,
                        double* pPressure, double* pSoundSpeed2, double* pBulkmod,
                        double* pTemperature) {

        /* The allocate function always allocates on the default device. The default device
         * can be set by calling setDefaultDataAllocator. Otherwise we can explicitly control
         * the location of the data by calling allocate(size, AMSDevice).
         */
        bool* p_ml_acceptable = static_cast<bool*>(AMS::utilities::allocate(num_data * sizeof(bool)));

        // ---------------------------------------------------------------------
        // operate directly on pointers
        // ---------------------------------------------------------------------

        // -------------------------------------------------------------
        // STEP 1: call the hdcache to look at input uncertainties
        // to decide if making a ML inference makes sense
        // -------------------------------------------------------------
        std::cout << " go call: " << hdcaches[mat_idx] << "\n";

        // ideally, we should do step 1 and step 2 async!
        if (hdcaches[mat_idx] != nullptr) {
            //CALIPER(CALI_MARK_BEGIN("UQ_MODULE");)
            hdcaches[mat_idx]->print();
            hdcaches[mat_idx]->evaluate(num_data, {pDensity, pEnergy}, p_ml_acceptable);
            //CALIPER(CALI_MARK_END("UQ_MODULE");)
        }
        //std::cout << " force exit after hdcache call!\n";
        //exit(1);
        /*-------------------------------------------------------------
    // -------------------------------------------------------------
     STEP 2: let's call surrogate for everything
     ideally, we should do step 1 and step 2 async!
    // -------------------------------------------------------------
    */

        /*
     At this point I am puzzled with how allocations should be done
     in regards to packing. The worst case scenario and simlest policy
     would require "length" *("Num Input Vectors" + "Num Output Vectors" + 1).
     This can be fine in the case of CPU execution. It is definetely too high
     for GPU execution. I will start a partioning scheme that limits the memory
     usage to a user defined size "PARTITION_SIZE". Setting the size to length
     should operate as the worst case scenario.
    */

        int partitionElements = data_handler::computePartitionSize(2, 4);

        /*
        The way partioning is working now we can have "inbalance" across
       iterations. As we only check the "uq" vector for the next
       partionElements. Thus, the vectors will be filled in up to that size.
       However, most times the vector will be half-empty.
    */
        for (int pId = 0; pId < num_data; pId += partitionElements) {
            // Pointer values which store data values
            // to be computed using the eos function.
            int elements = std::min(partitionElements, num_data - pId);

            double *packed_density, *packed_energy, *packed_pressure, *packed_soundspeed2,
                *packed_bulkmod, *packed_temperature;

            packed_density =
                static_cast<double*>(AMS::utilities::allocate(elements * sizeof(double)));
            packed_energy = static_cast<double*>(AMS::utilities::allocate(elements * sizeof(double)));
            packed_pressure =
                static_cast<double*>(AMS::utilities::allocate(elements * sizeof(double)));
            packed_soundspeed2 =
                static_cast<double*>(AMS::utilities::allocate(elements * sizeof(double)));
            packed_bulkmod =
                static_cast<double*>(AMS::utilities::allocate(elements * sizeof(double)));
            packed_temperature =
                static_cast<double*>(AMS::utilities::allocate(elements * sizeof(double)));

            std::vector<double*> sparse_inputs({&pDensity[pId], &pEnergy[pId]});
            std::vector<double*> sparse_outputs(
                {&pPressure[pId], &pSoundSpeed2[pId], &pBulkmod[pId], &pTemperature[pId]});

            std::vector<double*> packed_inputs({packed_density, packed_energy});
            std::vector<double*> packed_outputs(
                {packed_pressure, packed_soundspeed2, packed_bulkmod, packed_temperature});

            bool* predicate = &p_ml_acceptable[pId];

            if (surrogates[mat_idx] != nullptr) {
                // STEP 2:
                // let's call surrogate for everything
                /*
         One of the benefits of the packing is that we indirectly limit the size
         of the model. As it will perform inference on up to "elements" points.
         Thus, we indirectly control the maximum memory of the model.
         */
                CALIPER(CALI_MARK_BEGIN("SURROGATE");)
                surrogates[mat_idx]->Eval(elements, sparse_inputs, sparse_outputs);
                CALIPER(CALI_MARK_END("SURROGATE");)

#ifdef __SURROGATE_DEBUG__
                // TODO: I will revisit the RMSE later. We need to compute it only
                // for point which we have low uncertainty.
                eoses[mat_idx]->computeRMSE(num_elems_for_mat * num_qpts, &d_dense_density(0, 0),
                                            &d_dense_energy(0, 0), &d_dense_pressure(0, 0),
                                            &d_dense_soundspeed2(0, 0), &d_dense_bulkmod(0, 0),
                                            &d_dense_temperature(0, 0));
#endif
            }

            // Here we pack. ""
            long packedElements =
                data_handler::pack(predicate, elements, sparse_inputs, packed_inputs);

            std::cout << std::setprecision(2)
                      << "Physis Computed elements / Surrogate computed elements "
                         "(Fraction) ["
                      << packedElements << "/" << elements - packedElements << " ("
                      << static_cast<double>(packedElements) / static_cast<double>(elements)
                      << ")]\n";

            // -------------------------------------------------------------
            // STEP 3: call physics module only where d_dense_need_phys = true
            CALIPER(CALI_MARK_BEGIN("PHYSICS MODULE");)
            eoses[mat_idx]->Eval(packedElements, packed_energy, packed_density, packed_pressure,
                                 packed_soundspeed2, packed_bulkmod, packed_temperature);
            CALIPER(CALI_MARK_END("PHYSICS MODULE");)

#ifdef __ENABLE_DB__
            // STEP 3b:
            // for d_dense_uq = False we store into DB.
            CALIPER(CALI_MARK_BEGIN("DBSTORE");)
            inputs = {packed_energy, packed_density};
            outputs = {packed_pressure, packed_soundspeed2, packed_bulkmod, packed_temperature};
            DB->Store(packedElements, 2, 4, inputs, outputs);
            CALIPER(CALI_MARK_END("DBSTORE");)
#endif

            data_handler::unpack(predicate, elements, packed_outputs, sparse_outputs);

            // Deallocate temporal data
            AMS::utilities::deallocate(packed_density);
            AMS::utilities::deallocate(packed_energy);
            AMS::utilities::deallocate(packed_pressure);
            AMS::utilities::deallocate(packed_soundspeed2);
            AMS::utilities::deallocate(packed_bulkmod);
            AMS::utilities::deallocate(packed_temperature);
        }

        AMS::utilities::deallocate(p_ml_acceptable);
    }

    void evaluate(mfem::DenseTensor& density, mfem::DenseTensor& energy,
                  mfem::Array<int>& sparse_elem_indices, mfem::DenseTensor& pressure,
                  mfem::DenseTensor& soundspeed2, mfem::DenseTensor& bulkmod,
                  mfem::DenseTensor& temperature) {
        CALIPER(CALI_MARK_FUNCTION_BEGIN;)

        std::cout << "miniapp::evaluate: \n";
        // move/allocate data on the device.
        // if the data is already on the device this is basically a noop
        const auto d_density = RESHAPE_TENSOR(density, Read);
        const auto d_energy = RESHAPE_TENSOR(energy, Read);
        const auto d_pressure = RESHAPE_TENSOR(pressure, Write);
        const auto d_soundspeed2 = RESHAPE_TENSOR(soundspeed2, Write);
        const auto d_bulkmod = RESHAPE_TENSOR(bulkmod, Write);
        const auto d_temperature = RESHAPE_TENSOR(temperature, Write);

        const auto d_sparse_elem_indices =
            mfem::Reshape(sparse_elem_indices.Write(), sparse_elem_indices.Size());

        // ---------------------------------------------------------------------
        // for each material
        for (int mat_idx = 0; mat_idx < num_mats; ++mat_idx) {
            const int offset_curr = mat_idx == 0 ? num_mats : sparse_elem_indices[mat_idx - 1];
            const int offset_next = sparse_elem_indices[mat_idx];

            const int num_elems_for_mat = offset_next - offset_curr;
            if (num_elems_for_mat == 0) {
                continue;
            }

            // -----------------------------------------------------------------
            // NOTE: we've found it's faster to do sparse lookups on GPUs but on CPUs
            // the dense packing->looked->unpacking is better if we're using expensive
            // eoses. in the future we may just use dense representations everywhere
            // but for now we use sparse ones.
            if (is_cpu && pack_sparse_mats && num_elems_for_mat < num_elems) {
                std::cout << " material " << mat_idx << ": using sparse packing for "
                          << num_elems_for_mat << " elems\n";

                // -------------------------------------------------------------
                // TODO: I think Tom mentiond we can allocate these outside the loop
                // check again
                mfem::Array<double> dense_density(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_energy(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_pressure(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_soundspeed2(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_bulkmod(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_temperature(num_elems_for_mat * num_qpts);

                // these are device tensors!
                auto d_dense_density =
                    mfem::Reshape(dense_density.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_energy =
                    mfem::Reshape(dense_energy.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_pressure =
                    mfem::Reshape(dense_pressure.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_soundspeed2 =
                    mfem::Reshape(dense_soundspeed2.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_bulkmod =
                    mfem::Reshape(dense_bulkmod.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_temperature =
                    mfem::Reshape(dense_temperature.Write(), num_qpts, num_elems_for_mat);

                // -------------------------------------------------------------
                // sparse -> dense
                CALIPER(CALI_MARK_BEGIN("SPARSE_TO_DENSE");)
                data_handler::pack_ij(mat_idx, num_qpts, num_elems_for_mat, offset_curr,
                                      d_sparse_elem_indices, d_density, d_dense_density, d_energy,
                                      d_dense_energy);
                CALIPER(CALI_MARK_END("SPARSE_TO_DENSE");)
                // -------------------------------------------------------------

                evaluate_inner(mat_idx, num_elems_for_mat * num_qpts, &d_dense_density(0, 0),
                               &d_dense_energy(0, 0), &d_dense_pressure(0, 0),
                               &d_dense_soundspeed2(0, 0), &d_dense_bulkmod(0, 0),
                               &d_dense_temperature(0, 0));

                // -------------------------------------------------------------
                // dense -> sparse
                CALIPER(CALI_MARK_BEGIN("DENSE_TO_SPARSE");)
                data_handler::unpack_ij(mat_idx, num_qpts, num_elems_for_mat, offset_curr,
                                        d_sparse_elem_indices, d_dense_pressure, d_pressure,
                                        d_dense_soundspeed2, d_soundspeed2, d_dense_bulkmod,
                                        d_bulkmod, d_dense_temperature, d_temperature);
                CALIPER(CALI_MARK_END("DENSE_TO_SPARSE");)
                // -------------------------------------------------------------

            } else {
                evaluate_inner(mat_idx, num_elems * num_qpts,
                               const_cast<double*>(&d_density(0, 0, mat_idx)),
                               const_cast<double*>(&d_energy(0, 0, mat_idx)),
                               &d_pressure(0, 0, mat_idx), &d_soundspeed2(0, 0, mat_idx),
                               &d_bulkmod(0, 0, mat_idx), &d_temperature(0, 0, mat_idx));
            }
        }

        CALIPER(CALI_MARK_FUNCTION_END);
    }

    // --------------------------------------------------------------------------------
    void evaluate_orig(mfem::DenseTensor& density, mfem::DenseTensor& energy,
                       mfem::Array<int>& sparse_elem_indices, mfem::DenseTensor& pressure,
                       mfem::DenseTensor& soundspeed2, mfem::DenseTensor& bulkmod,
                       mfem::DenseTensor& temperature) {
        auto& rm = umpire::ResourceManager::getInstance();

        CALIPER(CALI_MARK_FUNCTION_BEGIN;)

        // move/allocate data on the device.
        // if the data is already on the device this is basically a noop
        const auto d_density = RESHAPE_TENSOR(density, Read);
        const auto d_energy = RESHAPE_TENSOR(energy, Read);
        const auto d_pressure = RESHAPE_TENSOR(pressure, Write);
        const auto d_soundspeed2 = RESHAPE_TENSOR(soundspeed2, Write);
        const auto d_bulkmod = RESHAPE_TENSOR(bulkmod, Write);
        const auto d_temperature = RESHAPE_TENSOR(temperature, Write);

        const auto d_sparse_elem_indices =
            mfem::Reshape(sparse_elem_indices.Write(), sparse_elem_indices.Size());

        // ---------------------------------------------------------------------
        // for each material
        for (int mat_idx = 0; mat_idx < num_mats; ++mat_idx) {
            const int offset_curr = mat_idx == 0 ? num_mats : sparse_elem_indices[mat_idx - 1];
            const int offset_next = sparse_elem_indices[mat_idx];

            const int num_elems_for_mat = offset_next - offset_curr;
            if (num_elems_for_mat == 0) {
                continue;
            }

            // -----------------------------------------------------------------
            // NOTE: we've found it's faster to do sparse lookups on GPUs but on CPUs
            // the dense packing->looked->unpacking is better if we're using expensive
            // eoses. in the future we may just use dense representations everywhere
            // but for now we use sparse ones.
            if (is_cpu && pack_sparse_mats && num_elems_for_mat < num_elems) {
                std::cout << " material " << mat_idx << ": using sparse packing for "
                          << num_elems_for_mat << " elems\n";

                // -------------------------------------------------------------
                // TODO: I think Tom mentiond we can allocate these outside the loop
                // check again
                mfem::Array<double> dense_density(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_energy(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_pressure(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_soundspeed2(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_bulkmod(num_elems_for_mat * num_qpts);
                mfem::Array<double> dense_temperature(num_elems_for_mat * num_qpts);

                // these are device tensors!
                auto d_dense_density =
                    mfem::Reshape(dense_density.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_energy =
                    mfem::Reshape(dense_energy.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_pressure =
                    mfem::Reshape(dense_pressure.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_soundspeed2 =
                    mfem::Reshape(dense_soundspeed2.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_bulkmod =
                    mfem::Reshape(dense_bulkmod.Write(), num_qpts, num_elems_for_mat);
                auto d_dense_temperature =
                    mfem::Reshape(dense_temperature.Write(), num_qpts, num_elems_for_mat);

                mfem::Array<bool> dense_ml_acceptable(num_elems_for_mat * num_qpts);
                dense_ml_acceptable = false;
                auto d_dense_ml_acceptable =
                    mfem::Reshape(dense_ml_acceptable.Write(), num_qpts, num_elems_for_mat);

                // -------------------------------------------------------------
                // sparse -> dense
                CALIPER(CALI_MARK_BEGIN("SPARSE_TO_DENSE");)
#ifdef NEW_PACKING
                data_handler::pack_ij(mat_idx, num_qpts, num_elems_for_mat, offset_curr,
                                      d_sparse_elem_indices, d_density, d_dense_density, d_energy,
                                      d_dense_energy);

#else
                MFEM_FORALL(elem_idx, num_elems_for_mat, {
                    const int sparse_elem_idx = d_sparse_elem_indices[offset_curr + elem_idx];
                    for (int qpt_idx = 0; qpt_idx < num_qpts; ++qpt_idx) {
                        d_dense_density(qpt_idx, elem_idx) =
                            d_density(qpt_idx, sparse_elem_idx, mat_idx);
                        d_dense_energy(qpt_idx, elem_idx) =
                            d_energy(qpt_idx, sparse_elem_idx, mat_idx);
                    }
                });
#endif
                CALIPER(CALI_MARK_END("SPARSE_TO_DENSE");)

                // -------------------------------------------------------------
                // create for uq flags
                // ask Tom about the memory management for this
                // should we create this memory again and again?

                // Let's start working with pointers.
                double* pDensity = &d_dense_density(0, 0);
                double* pEnergy = &d_dense_energy(0, 0);

                bool* pMl_acceptable = &d_dense_ml_acceptable(0, 0);

                double* pPressure = &d_dense_pressure(0, 0);
                double* pSoundSpeed2 = &d_dense_soundspeed2(0, 0);
                double* pBulkmod = &d_dense_bulkmod(0, 0);
                double* pTemperature = &d_dense_temperature(0, 0);

                // -------------------------------------------------------------
                // STEP 1: call the hdcache to look at input uncertainties
                // to decide if making a ML inference makes sense

                // ideally, we should do step 1 and step 2 async!
                if (hdcaches[mat_idx] != nullptr) {
                    CALIPER(CALI_MARK_BEGIN("UQ_MODULE");)
                    hdcaches[mat_idx]->evaluate(num_elems_for_mat * num_qpts, {pDensity, pEnergy},
                                                pMl_acceptable);
                    CALIPER(CALI_MARK_END("UQ_MODULE");)
                }

                // -------------------------------------------------------------
                // STEP 2: let's call surrogate for everything

                // ideally, we should do step 1 and step 2 async!
                /*
         At this point I am puzzled with how allocations should be done
         in regards to packing. The worst case scenario and simlest policy
         would require "length" *("Num Input Vectors" + "Num Output Vectors" +
         1). This can be fine in the case of CPU execution. It is definetely too
         high for GPU execution. I will start a partioning scheme that limits
         the memory usage to a user defined size "PARTITION_SIZE". Setting the
         size to length should operate as the worst case scenario.
        */
                // We have 6 elements + a vector holding the index values
                int partitionElements = data_handler::computePartitionSize(2, 4);
                auto dataAllocator = rm.getAllocator(AMS::utilities::getHostAllocatorName());

                /*
            The way partioning is working now we can have "inbalance" across
           iterations. As we only check the "uq" vector for the next
           partionElements. Thus, the vectors will be filled in up to that size.
           However, most times the vector will be half-empty.
        */
                int num_data = num_elems_for_mat * num_qpts;
                for (int pId = 0; pId < num_elems_for_mat * num_qpts; pId += partitionElements) {
                    // Pointer values which store data values
                    // to be computed using the eos function.

                    int elements = std::min(partitionElements, num_data - pId);

                    double *packed_density, *packed_energy, *packed_pressure, *packed_soundspeed2,
                        *packed_bulkmod, *packed_temperature;

                    int* reIndex =
                        static_cast<int*>(dataAllocator.allocate(elements * sizeof(int)));
                    packed_density =
                        static_cast<double*>(dataAllocator.allocate(elements * sizeof(double)));
                    packed_energy =
                        static_cast<double*>(dataAllocator.allocate(elements * sizeof(double)));
                    packed_pressure =
                        static_cast<double*>(dataAllocator.allocate(elements * sizeof(double)));
                    packed_soundspeed2 =
                        static_cast<double*>(dataAllocator.allocate(elements * sizeof(double)));
                    packed_bulkmod =
                        static_cast<double*>(dataAllocator.allocate(elements * sizeof(double)));
                    packed_temperature =
                        static_cast<double*>(dataAllocator.allocate(elements * sizeof(double)));

                    std::vector<double*> sparse_inputs({&pDensity[pId], &pEnergy[pId]});
                    std::vector<double*> sparse_outputs(
                        {&pPressure[pId], &pSoundSpeed2[pId], &pBulkmod[pId], &pTemperature[pId]});

                    std::vector<double*> packed_inputs({packed_density, packed_energy});
                    std::vector<double*> packed_outputs(
                        {packed_pressure, packed_soundspeed2, packed_bulkmod, packed_temperature});

                    bool* predicate = &pMl_acceptable[pId];

                    if (surrogates[mat_idx] != nullptr) {
                        // STEP 2:
                        // let's call surrogate for everything

                        /*
             One of the benefits of the packing is that we indirectly limit the
             size of the model. As it will perform inference on up to "elements"
             points. Thus, we indirectly control the maximum memory of the
             model.
             */
                        CALIPER(CALI_MARK_BEGIN("SURROGATE");)
                        surrogates[mat_idx]->Eval(elements, sparse_inputs, sparse_outputs);
                        CALIPER(CALI_MARK_END("SURROGATE");)
#ifdef __SURROGATE_DEBUG__
                        // TODO: I will revisit the RMSE later. We need to compute it only
                        // for point which we have low uncertainty.
                        eoses[mat_idx]->computeRMSE(
                            num_elems_for_mat * num_qpts, &d_dense_density(0, 0),
                            &d_dense_energy(0, 0), &d_dense_pressure(0, 0),
                            &d_dense_soundspeed2(0, 0), &d_dense_bulkmod(0, 0),
                            &d_dense_temperature(0, 0));
#endif
                    }
                    // Here we pack. ""
                    long packedElements = data_handler::pack(predicate, reIndex, elements,
                                                             sparse_inputs, packed_inputs);

                    std::cout << "Physis Computed elements / Surrogate computed elements ["
                              << packedElements << "/" << elements - packedElements << "]\n";

                    // -------------------------------------------------------------
                    // STEP 3: call physics module only where d_dense_need_phys = true
                    CALIPER(CALI_MARK_BEGIN("PHYSICS MODULE");)
                    eoses[mat_idx]->Eval(packedElements, packed_energy, packed_density,
                                         packed_pressure, packed_soundspeed2, packed_bulkmod,
                                         packed_temperature);
                    CALIPER(CALI_MARK_END("PHYSICS MODULE");)

#ifdef __ENABLE_DB__
                    // STEP 3b:
                    // for d_dense_uq = False we store into DB.
                    CALIPER(CALI_MARK_BEGIN("DBSTORE");)
                    inputs = {packed_energy, packed_density};
                    outputs = {packed_pressure, packed_soundspeed2, packed_bulkmod,
                               packed_temperature};
                    DB->Store(packedElements, 2, 4, packed_inputs, packed_outputs);
                    CALIPER(CALI_MARK_END("DBSTORE");)
#endif

                    data_handler::unpack(reIndex, packedElements, packed_outputs, sparse_outputs);

                    // STEP 4: convert dense -> sparse
                    CALIPER(CALI_MARK_BEGIN("DENSE_TO_SPARSE");)
#ifdef NEW_PACKING
                    data_handler::unpack_ij(mat_idx, num_qpts, num_elems_for_mat, offset_curr,
                                            d_sparse_elem_indices, d_dense_pressure, d_pressure,
                                            d_dense_soundspeed2, d_soundspeed2, d_dense_bulkmod,
                                            d_bulkmod, d_dense_temperature, d_temperature);

#else
                    MFEM_FORALL(elem_idx, num_elems_for_mat, {
                        const int sparse_elem_idx = d_sparse_elem_indices[offset_curr + elem_idx];

                        for (int qpt_idx = 0; qpt_idx < num_qpts; ++qpt_idx) {
                            d_pressure(qpt_idx, sparse_elem_idx, mat_idx) =
                                d_dense_pressure(qpt_idx, elem_idx);
                            d_soundspeed2(qpt_idx, sparse_elem_idx, mat_idx) =
                                d_dense_soundspeed2(qpt_idx, elem_idx);
                            d_bulkmod(qpt_idx, sparse_elem_idx, mat_idx) =
                                d_dense_bulkmod(qpt_idx, elem_idx);
                            d_temperature(qpt_idx, sparse_elem_idx, mat_idx) =
                                d_dense_temperature(qpt_idx, elem_idx);
                        }
                    });
#endif
                    CALIPER(CALI_MARK_END("DENSE_TO_SPARSE");)
                    // Deallocate temporal data
                    dataAllocator.deallocate(packed_density);
                    dataAllocator.deallocate(packed_energy);
                    dataAllocator.deallocate(packed_pressure);
                    dataAllocator.deallocate(packed_soundspeed2);
                    dataAllocator.deallocate(packed_bulkmod);
                    dataAllocator.deallocate(packed_temperature);
                    dataAllocator.deallocate(reIndex);
                }
            } else {
                if (surrogates[mat_idx] != nullptr) {
                    double* inputs[] = {const_cast<double*>(&d_density(0, 0, mat_idx)),
                                        const_cast<double*>(&d_energy(0, 0, mat_idx))};
                    double* outputs[] = {const_cast<double*>(&d_pressure(0, 0, mat_idx)),
                                         const_cast<double*>(&d_soundspeed2(0, 0, mat_idx)),
                                         const_cast<double*>(&d_bulkmod(0, 0, mat_idx)),
                                         const_cast<double*>(&d_temperature(0, 0, mat_idx))};
                    CALIPER(CALI_MARK_BEGIN("SURROGATE");)
                    surrogates[mat_idx]->Eval(num_elems_for_mat * num_qpts, 2, 4, inputs, outputs);
                    CALIPER(CALI_MARK_END("SURROGATE");)
                }
#ifdef __SURROGATE_DEBUG__
//                eoses[mat_idx]->computeRMSE(num_elems_for_mat * num_qpts,
//                                                 &d_dense_density(0, 0),
//                                                 &d_dense_energy(0, 0),
//                                                 &d_dense_pressure(0, 0),
//                                                 &d_dense_soundspeed2(0, 0),
//                                                 &d_dense_bulkmod(0, 0),
//                                                 &d_dense_temperature(0, 0));
#endif
                std::cout << " material " << mat_idx << ": using dense packing for "
                          << num_elems_for_mat << " elems\n";
                eoses[mat_idx]->Eval(num_elems * num_qpts, &d_density(0, 0, mat_idx),
                                     &d_energy(0, 0, mat_idx), &d_pressure(0, 0, mat_idx),
                                     &d_soundspeed2(0, 0, mat_idx), &d_bulkmod(0, 0, mat_idx),
                                     &d_temperature(0, 0, mat_idx));
            }
        }

        CALIPER(CALI_MARK_FUNCTION_END);
    }
};

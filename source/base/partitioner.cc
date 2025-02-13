// ---------------------------------------------------------------------
//
// Copyright (C) 1999 - 2019 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

#include <deal.II/base/partitioner.h>
#include <deal.II/base/partitioner.templates.h>

DEAL_II_NAMESPACE_OPEN

namespace Utilities
{
  namespace MPI
  {
    Partitioner::Partitioner()
      : global_size(0)
      , local_range_data(
          std::pair<types::global_dof_index, types::global_dof_index>(0, 0))
      , n_ghost_indices_data(0)
      , n_import_indices_data(0)
      , n_ghost_indices_in_larger_set(0)
      , my_pid(0)
      , n_procs(1)
      , communicator(MPI_COMM_SELF)
      , have_ghost_indices(false)
    {}



    Partitioner::Partitioner(const unsigned int size)
      : global_size(size)
      , locally_owned_range_data(size)
      , local_range_data(
          std::pair<types::global_dof_index, types::global_dof_index>(0, size))
      , n_ghost_indices_data(0)
      , n_import_indices_data(0)
      , n_ghost_indices_in_larger_set(0)
      , my_pid(0)
      , n_procs(1)
      , communicator(MPI_COMM_SELF)
      , have_ghost_indices(false)
    {
      locally_owned_range_data.add_range(0, size);
      locally_owned_range_data.compress();
      ghost_indices_data.set_size(size);
    }



    Partitioner::Partitioner(const IndexSet &locally_owned_indices,
                             const IndexSet &ghost_indices_in,
                             const MPI_Comm  communicator_in)
      : global_size(
          static_cast<types::global_dof_index>(locally_owned_indices.size()))
      , n_ghost_indices_data(0)
      , n_import_indices_data(0)
      , n_ghost_indices_in_larger_set(0)
      , my_pid(0)
      , n_procs(1)
      , communicator(communicator_in)
      , have_ghost_indices(false)
    {
      set_owned_indices(locally_owned_indices);
      set_ghost_indices(ghost_indices_in);
    }



    Partitioner::Partitioner(const IndexSet &locally_owned_indices,
                             const MPI_Comm  communicator_in)
      : global_size(
          static_cast<types::global_dof_index>(locally_owned_indices.size()))
      , n_ghost_indices_data(0)
      , n_import_indices_data(0)
      , n_ghost_indices_in_larger_set(0)
      , my_pid(0)
      , n_procs(1)
      , communicator(communicator_in)
      , have_ghost_indices(false)
    {
      set_owned_indices(locally_owned_indices);
    }



    void
    Partitioner::reinit(const IndexSet &vector_space_vector_index_set,
                        const IndexSet &read_write_vector_index_set,
                        const MPI_Comm &communicator_in)
    {
      have_ghost_indices = false;
      communicator       = communicator_in;
      set_owned_indices(vector_space_vector_index_set);
      set_ghost_indices(read_write_vector_index_set);
    }



    void
    Partitioner::set_owned_indices(const IndexSet &locally_owned_indices)
    {
      if (Utilities::MPI::job_supports_mpi() == true)
        {
          my_pid  = Utilities::MPI::this_mpi_process(communicator);
          n_procs = Utilities::MPI::n_mpi_processes(communicator);
        }
      else
        {
          my_pid  = 0;
          n_procs = 1;
        }

      // set the local range
      Assert(locally_owned_indices.is_contiguous() == true,
             ExcMessage("The index set specified in locally_owned_indices "
                        "is not contiguous."));
      locally_owned_indices.compress();
      if (locally_owned_indices.n_elements() > 0)
        local_range_data =
          std::pair<types::global_dof_index, types::global_dof_index>(
            locally_owned_indices.nth_index_in_set(0),
            locally_owned_indices.nth_index_in_set(0) +
              locally_owned_indices.n_elements());
      AssertThrow(
        local_range_data.second - local_range_data.first <
          static_cast<types::global_dof_index>(
            std::numeric_limits<unsigned int>::max()),
        ExcMessage(
          "Index overflow: This class supports at most 2^32-1 locally owned vector entries"));
      locally_owned_range_data.set_size(locally_owned_indices.size());
      locally_owned_range_data.add_range(local_range_data.first,
                                         local_range_data.second);
      locally_owned_range_data.compress();

      ghost_indices_data.set_size(locally_owned_indices.size());
    }



    void
    Partitioner::set_ghost_indices(const IndexSet &ghost_indices_in,
                                   const IndexSet &larger_ghost_index_set)
    {
      // Set ghost indices from input. To be sure that no entries from the
      // locally owned range are present, subtract the locally owned indices
      // in any case.
      Assert(ghost_indices_in.n_elements() == 0 ||
               ghost_indices_in.size() == locally_owned_range_data.size(),
             ExcDimensionMismatch(ghost_indices_in.size(),
                                  locally_owned_range_data.size()));

      ghost_indices_data = ghost_indices_in;
      if (ghost_indices_data.size() != locally_owned_range_data.size())
        ghost_indices_data.set_size(locally_owned_range_data.size());
      ghost_indices_data.subtract_set(locally_owned_range_data);
      ghost_indices_data.compress();
      AssertThrow(
        ghost_indices_data.n_elements() <
          static_cast<types::global_dof_index>(
            std::numeric_limits<unsigned int>::max()),
        ExcMessage(
          "Index overflow: This class supports at most 2^32-1 ghost elements"));
      n_ghost_indices_data = ghost_indices_data.n_elements();

      have_ghost_indices =
        Utilities::MPI::sum(n_ghost_indices_data, communicator) > 0;

      // In the rest of this function, we determine the point-to-point
      // communication pattern of the partitioner. We make up a list with both
      // the processors the ghost indices actually belong to, and the indices
      // that are locally held but ghost indices of other processors. This
      // allows then to import and export data very easily.

      // find out the end index for each processor and communicate it (this
      // implies the start index for the next processor)
#ifdef DEAL_II_WITH_MPI
      if (n_procs < 2)
        {
          Assert(ghost_indices_data.n_elements() == 0, ExcInternalError());
          Assert(n_import_indices_data == 0, ExcInternalError());
          Assert(n_ghost_indices_data == 0, ExcInternalError());
          return;
        }

      std::vector<types::global_dof_index> first_index(n_procs + 1);
      // Allow non-zero start index for the vector. send this data to all
      // processors
      first_index[0] = local_range_data.first;
      int ierr       = MPI_Bcast(
        first_index.data(), 1, DEAL_II_DOF_INDEX_MPI_TYPE, 0, communicator);
      AssertThrowMPI(ierr);

      // Get the end-of-local_range for all processors
      ierr = MPI_Allgather(&local_range_data.second,
                           1,
                           DEAL_II_DOF_INDEX_MPI_TYPE,
                           &first_index[1],
                           1,
                           DEAL_II_DOF_INDEX_MPI_TYPE,
                           communicator);
      AssertThrowMPI(ierr);
      first_index[n_procs] = global_size;

      // fix case when there are some processors without any locally owned
      // indices: then there might be a zero in some entries. The reason
      // is that local_range_data will contain [0,0) and second index is
      // incorrect inside the Allgather'ed first_index. Below we fix this
      // by ensuring that the start point is always the end index of the
      // processor immediately before.
      if (global_size > 0)
        {
          for (unsigned int i = 1; i < n_procs; ++i)
            if (first_index[i] == 0)
              first_index[i] = first_index[i - 1];

          // correct if our processor has a wrong local range
          if (first_index[my_pid] != local_range_data.first)
            {
              Assert(local_range_data.first == local_range_data.second,
                     ExcInternalError());
              local_range_data.first = local_range_data.second =
                first_index[my_pid];
            }
        }

      unsigned int n_ghost_targets = 0;
      {
        const auto index_owner =
          Utilities::MPI::compute_index_owner(this->locally_owned_range_data,
                                              this->ghost_indices_data,
                                              this->communicator);

        ghost_targets_data.clear();

        if (index_owner.size() > 0)
          {
            ghost_targets_data.emplace_back(index_owner[0], 0);
            for (auto i : index_owner)
              {
                Assert(i >= ghost_targets_data.back().first,
                       ExcInternalError(
                         "Expect result of compute_index_owner to be sorted"));
                if (i == ghost_targets_data.back().first)
                  ghost_targets_data.back().second++;
                else
                  ghost_targets_data.emplace_back(i, 1);
              }
          }

        n_ghost_targets = ghost_targets_data.size();
      }
      // find the processes that want to import to me
      {
        std::vector<int> send_buffer(n_procs, 0);
        std::vector<int> receive_buffer(n_procs, 0);
        for (unsigned int i = 0; i < n_ghost_targets; i++)
          send_buffer[ghost_targets_data[i].first] =
            ghost_targets_data[i].second;

        const int ierr = MPI_Alltoall(send_buffer.data(),
                                      1,
                                      MPI_INT,
                                      receive_buffer.data(),
                                      1,
                                      MPI_INT,
                                      communicator);
        AssertThrowMPI(ierr);

        // allocate memory for import data
        std::vector<std::pair<unsigned int, unsigned int>> import_targets_temp;
        n_import_indices_data = 0;
        for (unsigned int i = 0; i < n_procs; i++)
          if (receive_buffer[i] > 0)
            {
              n_import_indices_data += receive_buffer[i];
              import_targets_temp.emplace_back(i, receive_buffer[i]);
            }
        // copy, don't move, to get deterministic memory usage.
        import_targets_data = import_targets_temp;
      }

      // now that we know how many indices each process will receive from
      // ghosts, send and receive indices for import data. non-blocking receives
      // and blocking sends
      {
        std::vector<types::global_dof_index> expanded_import_indices(
          n_import_indices_data);
        unsigned int             current_index_start = 0;
        std::vector<MPI_Request> import_requests(import_targets_data.size() +
                                                 n_ghost_targets);
        for (unsigned int i = 0; i < import_targets_data.size(); i++)
          {
            const int ierr =
              MPI_Irecv(&expanded_import_indices[current_index_start],
                        import_targets_data[i].second,
                        DEAL_II_DOF_INDEX_MPI_TYPE,
                        import_targets_data[i].first,
                        import_targets_data[i].first,
                        communicator,
                        &import_requests[i]);
            AssertThrowMPI(ierr);
            current_index_start += import_targets_data[i].second;
          }
        AssertDimension(current_index_start, n_import_indices_data);

        // use non-blocking send for ghost indices stored in
        // expanded_ghost_indices
        std::vector<types::global_dof_index> expanded_ghost_indices;
        if (n_ghost_indices_data > 0)
          ghost_indices_data.fill_index_vector(expanded_ghost_indices);

        current_index_start = 0;
        for (unsigned int i = 0; i < n_ghost_targets; i++)
          {
            const int ierr =
              MPI_Isend(&expanded_ghost_indices[current_index_start],
                        ghost_targets_data[i].second,
                        DEAL_II_DOF_INDEX_MPI_TYPE,
                        ghost_targets_data[i].first,
                        my_pid,
                        communicator,
                        &import_requests[import_targets_data.size() + i]);
            AssertThrowMPI(ierr);
            current_index_start += ghost_targets_data[i].second;
          }
        AssertDimension(current_index_start, n_ghost_indices_data);

        // wait for all import from other processes to be done
        if (import_requests.size() > 0)
          {
            const int ierr = MPI_Waitall(import_requests.size(),
                                         import_requests.data(),
                                         MPI_STATUSES_IGNORE);
            AssertThrowMPI(ierr);
          }

        // transform import indices to local index space and compress
        // contiguous indices in form of ranges
        {
          import_indices_chunks_by_rank_data.resize(import_targets_data.size() +
                                                    1);
          import_indices_chunks_by_rank_data[0] = 0;
          // a vector which stores import indices as ranges [a_i,b_i)
          std::vector<std::pair<unsigned int, unsigned int>>
                       compressed_import_indices;
          unsigned int shift = 0;
          for (unsigned int p = 0; p < import_targets_data.size(); ++p)
            {
              types::global_dof_index last_index =
                numbers::invalid_dof_index - 1;
              for (unsigned int ii = 0; ii < import_targets_data[p].second;
                   ++ii)
                {
                  // index in expanded_import_indices for a pair (p,ii):
                  const unsigned int i = shift + ii;
                  Assert(expanded_import_indices[i] >= local_range_data.first &&
                           expanded_import_indices[i] < local_range_data.second,
                         ExcIndexRange(expanded_import_indices[i],
                                       local_range_data.first,
                                       local_range_data.second));
                  // local index starting from the beginning of locally owned
                  // DoFs:
                  types::global_dof_index new_index =
                    (expanded_import_indices[i] - local_range_data.first);
                  Assert(new_index < numbers::invalid_unsigned_int,
                         ExcNotImplemented());
                  if (new_index == last_index + 1)
                    // if contiguous, increment the end of last range:
                    compressed_import_indices.back().second++;
                  else
                    // otherwise start a new range:
                    compressed_import_indices.emplace_back(new_index,
                                                           new_index + 1);
                  last_index = new_index;
                }
              shift += import_targets_data[p].second;
              import_indices_chunks_by_rank_data[p + 1] =
                compressed_import_indices.size();
            }
          import_indices_data = compressed_import_indices;

          // sanity check
#  ifdef DEBUG
          const types::global_dof_index n_local_dofs =
            local_range_data.second - local_range_data.first;
          for (const auto &range : import_indices_data)
            {
              AssertIndexRange(range.first, n_local_dofs);
              AssertIndexRange(range.second - 1, n_local_dofs);
            }
#  endif
        }
      }
#endif // #ifdef DEAL_II_WITH_MPI

      if (larger_ghost_index_set.size() == 0)
        {
          ghost_indices_subset_chunks_by_rank_data.clear();
          ghost_indices_subset_data.emplace_back(local_size(),
                                                 local_size() +
                                                   n_ghost_indices());
          n_ghost_indices_in_larger_set = n_ghost_indices_data;
        }
      else
        {
          AssertDimension(larger_ghost_index_set.size(),
                          ghost_indices_data.size());
          Assert(
            (larger_ghost_index_set & locally_owned_range_data).n_elements() ==
              0,
            ExcMessage("Ghost index set should not overlap with owned set."));
          Assert((larger_ghost_index_set & ghost_indices_data) ==
                   ghost_indices_data,
                 ExcMessage("Larger ghost index set must contain the tight "
                            "ghost index set."));

          n_ghost_indices_in_larger_set = larger_ghost_index_set.n_elements();

          // first translate tight ghost indices into indices within the large
          // set:
          std::vector<unsigned int> expanded_numbering;
          for (dealii::IndexSet::size_type index : ghost_indices_data)
            {
              Assert(larger_ghost_index_set.is_element(index),
                     ExcMessage("The given larger ghost index set must contain "
                                "all indices in the actual index set."));
              Assert(
                larger_ghost_index_set.index_within_set(index) <
                  static_cast<types::global_dof_index>(
                    std::numeric_limits<unsigned int>::max()),
                ExcMessage(
                  "Index overflow: This class supports at most 2^32-1 ghost elements"));
              expanded_numbering.push_back(
                larger_ghost_index_set.index_within_set(index));
            }

          // now rework expanded_numbering into ranges and store in:
          std::vector<std::pair<unsigned int, unsigned int>>
            ghost_indices_subset;
          ghost_indices_subset_chunks_by_rank_data.resize(
            ghost_targets_data.size() + 1);
          // also populate ghost_indices_subset_chunks_by_rank_data
          ghost_indices_subset_chunks_by_rank_data[0] = 0;
          unsigned int shift                          = 0;
          for (unsigned int p = 0; p < ghost_targets_data.size(); ++p)
            {
              unsigned int last_index = numbers::invalid_unsigned_int - 1;
              for (unsigned int ii = 0; ii < ghost_targets_data[p].second; ii++)
                {
                  const unsigned int i = shift + ii;
                  if (expanded_numbering[i] == last_index + 1)
                    // if contiguous, increment the end of last range:
                    ghost_indices_subset.back().second++;
                  else
                    // otherwise start a new range
                    ghost_indices_subset.emplace_back(expanded_numbering[i],
                                                      expanded_numbering[i] +
                                                        1);
                  last_index = expanded_numbering[i];
                }
              shift += ghost_targets_data[p].second;
              ghost_indices_subset_chunks_by_rank_data[p + 1] =
                ghost_indices_subset.size();
            }
          ghost_indices_subset_data = ghost_indices_subset;
        }
    }



    bool
    Partitioner::is_compatible(const Partitioner &part) const
    {
      // if the partitioner points to the same memory location as the calling
      // processor
      if (&part == this)
        return true;
#ifdef DEAL_II_WITH_MPI
      if (Utilities::MPI::job_supports_mpi())
        {
          int       communicators_same = 0;
          const int ierr               = MPI_Comm_compare(part.communicator,
                                            communicator,
                                            &communicators_same);
          AssertThrowMPI(ierr);
          if (!(communicators_same == MPI_IDENT ||
                communicators_same == MPI_CONGRUENT))
            return false;
        }
#endif
      return (global_size == part.global_size &&
              local_range_data == part.local_range_data &&
              ghost_indices_data == part.ghost_indices_data);
    }



    bool
    Partitioner::is_globally_compatible(const Partitioner &part) const
    {
      return Utilities::MPI::min(static_cast<int>(is_compatible(part)),
                                 communicator) == 1;
    }



    std::size_t
    Partitioner::memory_consumption() const
    {
      std::size_t memory = (3 * sizeof(types::global_dof_index) +
                            4 * sizeof(unsigned int) + sizeof(MPI_Comm));
      memory += MemoryConsumption::memory_consumption(locally_owned_range_data);
      memory += MemoryConsumption::memory_consumption(ghost_targets_data);
      memory += MemoryConsumption::memory_consumption(import_targets_data);
      memory += MemoryConsumption::memory_consumption(import_indices_data);
      memory += MemoryConsumption::memory_consumption(
        import_indices_chunks_by_rank_data);
      memory += MemoryConsumption::memory_consumption(
        ghost_indices_subset_chunks_by_rank_data);
      memory +=
        MemoryConsumption::memory_consumption(ghost_indices_subset_data);
      memory += MemoryConsumption::memory_consumption(ghost_indices_data);
      return memory;
    }

  } // end of namespace MPI

} // end of namespace Utilities



// explicit instantiations from .templates.h file
#include "partitioner.inst"

DEAL_II_NAMESPACE_CLOSE

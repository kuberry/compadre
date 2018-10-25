#include "GMLS.hpp"
#include "GMLS_ApplyTargetEvaluations.hpp"
#include "GMLS_Basis.hpp"
#include "GMLS_Quadrature.hpp"
#include "GMLS_Targets.hpp"

#include <float.h> // for DBL_MAX

#ifdef COMPADRE_USE_KOKKOSCORE

void GMLS::generateAlphas() {
/*
	 *	Operations to Device
	 */

	// copy over operations
	_operations = Kokkos::View<ReconstructionOperator::TargetOperation*> ("operations", _lro.size());
	_host_operations = Kokkos::create_mirror_view(_operations);
	
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;
	
	// loop through list of linear reconstruction operations to be performed and set them on the host
	for (int i=0; i<_lro.size(); ++i) _host_operations(i) = _lro[i];

	// get copy of operations on the device
	Kokkos::deep_copy(_operations, _host_operations);

	/*
	 *	Initialize Alphas and Prestencil Weights
	 */

	// initialize all alpha values to be used for taking the dot product with data to get a reconstruction 
	_alphas = Kokkos::View<double**, layout_type>("coefficients", ORDER_INDICES(_neighbor_lists.dimension_0(), _total_alpha_values*max_num_neighbors));

	// initialize weights to be applied to data in order to get it into the form expected as a sample
	if (_data_sampling_functional != ReconstructionOperator::SamplingFunctional::PointSample) {
		_prestencil_weights = Kokkos::View<double**, layout_type>("prestencil weights", _neighbor_lists.dimension_0(), std::pow(_dimensions,ReconstructionOperator::SamplingOutputTensorRank[_data_sampling_functional]+1)*2*max_num_neighbors, 0);
	}

	/*
	 *	Determine if Nontrivial Null Space in Solution
	 */

	// check whether the sampling function acting on the basis will induce a nontrivial nullspace
	// an example would be reconstructing from gradient information, which would annihilate constants
	if (ReconstructionOperator::SamplingNontrivialNullspace[_polynomial_sampling_functional]==1) {
		_nontrivial_nullspace = true;
	}

	/*
	 *	Determine if Nonstandard Sampling Dimension or Basis Component Dimension
	 */

	// calculate sampling dimension 
	// for example calculating a vector field on a manifold all at once, rather than component by component
	if (ReconstructionOperator::SamplingOutputTensorRank[_data_sampling_functional] > 0) {
		if (_dense_solver_type == ReconstructionOperator::DenseSolverType::MANIFOLD) {
			_sampling_multiplier = _dimensions-1;
		} else {
			_sampling_multiplier = _dimensions;
		}
	} else {
		_sampling_multiplier = 1;
	}

	// calculate the dimension of the basis (a vector space on a manifold requires two components, for example)
	if (ReconstructionOperator::ReconstructionSpaceRank[_reconstruction_space] > 0) {
		if (_dense_solver_type == ReconstructionOperator::DenseSolverType::MANIFOLD) {
			_basis_multiplier = _dimensions-1;
		} else {
			_basis_multiplier = _dimensions;
		}
	} else {
		_basis_multiplier = 1;
	}

	// special case for using a higher order for sampling from a polynomial space that are gradients of a scalar polynomial
	if (_polynomial_sampling_functional == ReconstructionOperator::SamplingFunctional::StaggeredEdgeAnalyticGradientIntegralSample) {
		// if the reconstruction is being made with a gradient of a basis, then we want that basis to be one order higher so that
		// the gradient is consistent with the convergence order expected.
		_poly_order += 1;
	}

	/*
	 *	Dimensions
	 */

	// for tallying scratch space needed for device kernel calls
	int team_scratch_size_a = 0;

	// TEMPORARY, take to zero after conversion
	int team_scratch_size_b = scratch_vector_type::shmem_size(max_num_neighbors*_sampling_multiplier); // weights W;
	int thread_scratch_size_a = 0;
	int thread_scratch_size_b = 0;

	// dimensions that are relevant for each subproblem
	int max_num_rows = _sampling_multiplier*max_num_neighbors;
	int this_num_columns = _basis_multiplier*_NP;
	int max_matrix_dimension = max_num_rows > this_num_columns
		? max_num_rows : this_num_columns;

	if (_dense_solver_type == ReconstructionOperator::DenseSolverType::MANIFOLD) {
		// these dimensions already calculated differ in the case of manifolds
		const int manifold_NP = this->getNP(_manifold_poly_order, _dimensions-1);
		const int target_NP = this->getNP(_poly_order, _dimensions-1);
		const int max_manifold_NP = (manifold_NP > target_NP) ? manifold_NP : target_NP;
		const int max_NP = (max_manifold_NP > _NP) ? max_manifold_NP : _NP;
		this_num_columns = _basis_multiplier*max_manifold_NP;
		max_matrix_dimension = (max_num_neighbors*_sampling_multiplier > max_NP*_basis_multiplier)
			? max_num_neighbors*_sampling_multiplier : max_NP*_basis_multiplier;
		const int max_P_row_size = ((_dimensions-1)*manifold_NP > max_NP*_total_alpha_values*_basis_multiplier) ? (_dimensions-1)*manifold_NP : max_NP*_total_alpha_values*_basis_multiplier;

		/*
		 *	Calculate Scratch Space Allocations
		 */

		team_scratch_size_b += scratch_matrix_type::shmem_size(_dimensions-1, _dimensions-1); // G
		team_scratch_size_b += scratch_matrix_type::shmem_size(_dimensions, _dimensions); // PTP matrix
		team_scratch_size_b += scratch_vector_type::shmem_size( (_dimensions-1)*max_num_neighbors ); // manifold_gradient

		team_scratch_size_b += scratch_vector_type::shmem_size(max_num_neighbors*std::max(_sampling_multiplier,_basis_multiplier)); // t1 work vector for qr
		team_scratch_size_b += scratch_vector_type::shmem_size(max_num_neighbors*std::max(_sampling_multiplier,_basis_multiplier)); // t2 work vector for qr
		team_scratch_size_b += scratch_vector_type::shmem_size(1); // t3 work vector for qr

		team_scratch_size_b += scratch_matrix_type::shmem_size(max_P_row_size, _sampling_multiplier); // row of P matrix, one for each operator
		thread_scratch_size_b += scratch_vector_type::shmem_size(max_NP*_basis_multiplier); // delta, used for each thread

		team_scratch_size_b += scratch_vector_type::shmem_size(max_manifold_NP*_basis_multiplier); // S
		team_scratch_size_b += scratch_matrix_type::shmem_size(max_manifold_NP*_basis_multiplier, _sampling_multiplier); // b_data, used for eachM_data team
		team_scratch_size_b += scratch_matrix_type::shmem_size(max_manifold_NP*_basis_multiplier, max_manifold_NP*_basis_multiplier); // Vsvd


		// allocate matrices on the device
		_V = Kokkos::View<double*>("V",_target_coordinates.dimension_0()*_dimensions*_dimensions);
		_T = Kokkos::View<double*>("T",_target_coordinates.dimension_0()*_dimensions*(_dimensions-1));
		_manifold_metric_tensor_inverse = Kokkos::View<double*>("manifold metric tensor inverse",_target_coordinates.dimension_0()*(_dimensions-1)*(_dimensions-1));
		_manifold_curvature_coefficients = Kokkos::View<double*>("manifold curvature coefficients",_target_coordinates.dimension_0()*manifold_NP);
		_manifold_curvature_gradient = Kokkos::View<double*>("manifold curvature gradient",_target_coordinates.dimension_0()*(_dimensions-1));

		// set to zero
		Kokkos::deep_copy(_V, 0);
		Kokkos::deep_copy(_T, 0);
		Kokkos::deep_copy(_manifold_metric_tensor_inverse, 0);
		Kokkos::deep_copy(_manifold_curvature_coefficients, 0);
		Kokkos::deep_copy(_manifold_curvature_gradient, 0);

	} else  { // QR

		/*
		 *	Calculate Scratch Space Allocations
		 */

		team_scratch_size_a += scratch_vector_type::shmem_size(max_num_rows); // t1 work vector for qr
		team_scratch_size_a += scratch_vector_type::shmem_size(max_num_rows); // t2 work vector for qr

		// row of P matrix, one for each operator
		team_scratch_size_b += scratch_matrix_type::shmem_size(this_num_columns*_total_alpha_values, _sampling_multiplier); 

		thread_scratch_size_b += scratch_vector_type::shmem_size(this_num_columns); // delta, used for each thread
	}

	/*
	 *	Allocate Global Device Storage of Data Needed Over Multiple Calls
	 */

	// allocate matrices on the device
	_P = Kokkos::View<double*>("P",_target_coordinates.dimension_0()*max_matrix_dimension*max_matrix_dimension);
	_RHS = Kokkos::View<double*>("RHS",_target_coordinates.dimension_0()*max_num_rows*max_num_rows);
	_w = Kokkos::View<double*>("w",_target_coordinates.dimension_0()*max_num_rows);

	// set to zero
	Kokkos::deep_copy(_P, 0);
	Kokkos::deep_copy(_RHS, 0);
	Kokkos::deep_copy(_w, 0);
	
	/*
	 *	Calculate Optimal Threads Based On Levels of Parallelism
	 */

#ifdef COMPADRE_USE_CUDA
	int threads_per_team = 32;
	if (_basis_multiplier*_NP > 96) threads_per_team += 32;
#else
	int threads_per_team = 1;
#endif
	Kokkos::fence();


	if (_dense_solver_type == ReconstructionOperator::DenseSolverType::MANIFOLD) {

		/*
		 *	MANIFOLD Problems
		 */

		// generate quadrature for staggered approach
		this->generate1DQuadrature();

		// coarse tangent plane approximation
		this->CallFunctorWithTeamThreads<ComputeCoarseTangentPlane>(threads_per_team, team_scratch_size_a, team_scratch_size_b, thread_scratch_size_a, thread_scratch_size_b);

		// construct curvature approximation and store relevant geometric data
		// not here yet

		// assembles the P*sqrt(weights) matrix and constructs sqrt(weights)*Identity
		this->CallFunctorWithTeamThreads<AssembleManifoldPsqrtW>(threads_per_team, team_scratch_size_a, team_scratch_size_b, thread_scratch_size_a, thread_scratch_size_b);

		if (_nontrivial_nullspace) {
			Kokkos::Profiling::pushRegion("Manifold SVD Factorization");
			GMLS_LinearAlgebra::batchSVDFactorize(_P.ptr_on_device(), _RHS.ptr_on_device(), max_matrix_dimension, max_num_rows, this_num_columns, _target_coordinates.dimension_0());
			Kokkos::Profiling::popRegion();
		} else {
			Kokkos::Profiling::pushRegion("Manifold QR Factorization");
			GMLS_LinearAlgebra::batchQRFactorize(_P.ptr_on_device(), _RHS.ptr_on_device(), max_matrix_dimension, max_num_rows, this_num_columns, _target_coordinates.dimension_0());
			Kokkos::Profiling::popRegion();
		}

		// evaluates targets, applies target evaluation to polynomial coefficients to store in _alphas
		this->CallFunctorWithTeamThreads<ApplyManifoldTargets>(threads_per_team, team_scratch_size_a, team_scratch_size_b, thread_scratch_size_a, thread_scratch_size_b);

		// calculate prestencil weights
		this->CallFunctorWithTeamThreads<ComputePrestencilWeights>(threads_per_team, team_scratch_size_a, team_scratch_size_b, thread_scratch_size_a, thread_scratch_size_b);

	} else {

		/*
		 *	STANDARD GMLS Problems
		 */

		// assembles the P*sqrt(weights) matrix and constructs sqrt(weights)*Identity
		this->CallFunctorWithTeamThreads<AssembleStandardPsqrtW>(threads_per_team, team_scratch_size_a, team_scratch_size_b, thread_scratch_size_a, thread_scratch_size_b);
		Kokkos::fence();

		if (_dense_solver_type == ReconstructionOperator::DenseSolverType::QR) {
			Kokkos::Profiling::pushRegion("QR Factorization");
			GMLS_LinearAlgebra::batchQRFactorize(_P.ptr_on_device(), _RHS.ptr_on_device(), max_matrix_dimension, max_num_rows, this_num_columns, _target_coordinates.dimension_0());
			Kokkos::Profiling::popRegion();
		} else if (_dense_solver_type == ReconstructionOperator::DenseSolverType::LU) {
			//Kokkos::Profiling::pushRegion("LU Factorization");
			//GMLS_LinearAlgebra::batchLUFactorize(_P.ptr_on_device(), _RHS.ptr_on_device(), max_matrix_dimension, this_num_columns, _target_coordinates.dimension_0());
			//Kokkos::Profiling::popRegion();
		}
		Kokkos::fence();

		// evaluates targets, applies target evaluation to polynomial coefficients to store in _alphas
		this->CallFunctorWithTeamThreads<ApplyStandardTargets>(threads_per_team, team_scratch_size_a, team_scratch_size_b, thread_scratch_size_a, thread_scratch_size_b);

	}
	Kokkos::fence();

	/*
	 *	Device to Host Copy Of Solution
	 */

	// copy computed alphas back to the host
	_host_alphas = Kokkos::create_mirror_view(_alphas);
	if (_data_sampling_functional != ReconstructionOperator::SamplingFunctional::PointSample)
		_host_prestencil_weights = Kokkos::create_mirror_view(_prestencil_weights);
	Kokkos::deep_copy(_host_alphas, _alphas);
	Kokkos::fence();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const AssembleStandardPsqrtW&, const member_type& teamMember) const {

	/*
	 *	Dimensions
	 */

	const int target_index = teamMember.league_rank();
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;

	const int max_num_rows = _sampling_multiplier*max_num_neighbors;
	const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
	const int this_num_columns = _basis_multiplier*_NP;
	const int max_matrix_dimension = max_num_rows > this_num_columns
			? max_num_rows : this_num_columns;

	/*
	 *	Data
	 */

	// team_scratch all have a copy each per team
	// the threads in a team work on these local copies that only the team sees
	// thread_scratch has a copy per thread

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > PsqrtW(_P.data() + target_index*max_matrix_dimension*max_matrix_dimension, max_matrix_dimension, max_matrix_dimension);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > RHS(_RHS.data() + target_index*max_num_rows*max_num_rows, max_num_rows, max_num_rows);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > w(_w.data() + target_index*max_num_rows);

	scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_a), max_num_rows);
	scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_a), max_num_rows);

	// delta is a temporary variable that preallocates space on which solutions of size _basis_multiplier*_NP will
	// be computed. This allows the gpu to allocate the memory in advance
	scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), this_num_columns);

	/*
	 *	Assemble P*sqrt(W) and sqrt(w)*Identity
	 */

	// creates the matrix sqrt(W)*P
	this->createWeightsAndP(teamMember, delta, PsqrtW, w, _dimensions, _poly_order, true /*weight_p*/, NULL /*&V*/, NULL /*&T*/, _polynomial_sampling_functional);

#if not(defined(COMPADRE_USE_CUDA))
	// LAPACK calls use Fortran data layout so we need LayoutLeft but have LayoutRight
	GMLS_LinearAlgebra::matrixToLayoutLeft(teamMember, t1, t2, PsqrtW, this_num_columns, this_num_rows);
#endif

	// fill in RHS with Identity * sqrt(weights)
	Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,max_num_rows), [=] (const int i) {
		for(int j = 0; j < max_num_rows; ++j) {
			RHS(ORDER_INDICES(j,i)) = (i==j) ? std::sqrt(w(i)) : 0;
		}
	});

	teamMember.team_barrier();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ApplyStandardTargets&, const member_type& teamMember) const {

	/*
	 *	Dimensions
	 */

	const int target_index = teamMember.league_rank();
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;

	const int max_num_rows = _sampling_multiplier*max_num_neighbors;
	const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
	const int this_num_columns = _basis_multiplier*_NP;
	const int max_matrix_dimension = max_num_rows > this_num_columns
			? max_num_rows : this_num_columns;

	/*
	 *	Data
	 */

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > PsqrtW(_P.data() + target_index*max_matrix_dimension*max_matrix_dimension, max_matrix_dimension, max_matrix_dimension);
	// Coefficients for polynomial basis have overwritten _RHS
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > Coeffs(_RHS.data() + target_index*max_num_rows*max_num_rows, max_num_rows, max_num_rows);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > w(_w.data() + target_index*max_num_rows);

	scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_a), max_num_rows);
	scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_a), max_num_rows);
	scratch_matrix_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), this_num_columns*_total_alpha_values, _sampling_multiplier);

	/*
	 *	Apply Standard Target Evaluations to Polynomial Coefficients
	 */

	// get evaluation of target functionals
	Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
		this->computeTargetFunctionals(teamMember, t1, t2, P_target_row);
	});
	teamMember.team_barrier();

	this->applyQR(teamMember, t1, t2, Coeffs, PsqrtW, w, P_target_row, _NP); 
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ComputeCoarseTangentPlane&, const member_type& teamMember) const {

	/*
	 *	Dimensions
	 */

	const int target_index = teamMember.league_rank();
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;

	// these dimensions already calculated differ in the case of manifolds
	const int max_num_rows = _sampling_multiplier*max_num_neighbors;
	const int manifold_NP = this->getNP(_manifold_poly_order, _dimensions-1);
	const int target_NP = this->getNP(_poly_order, _dimensions-1);
	const int max_manifold_NP = (manifold_NP > target_NP) ? manifold_NP : target_NP;
	const int max_NP = (max_manifold_NP > _NP) ? max_manifold_NP : _NP;
	const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
	const int this_num_columns = _basis_multiplier*max_manifold_NP;
	const int max_matrix_dimension = (max_num_neighbors*_sampling_multiplier > max_NP*_basis_multiplier)
		? max_num_neighbors*_sampling_multiplier : max_NP*_basis_multiplier;
	// max_num_rows remains the same
	const int max_P_row_size = ((_dimensions-1)*manifold_NP > max_NP*_total_alpha_values*_basis_multiplier) ? (_dimensions-1)*manifold_NP : max_NP*_total_alpha_values*_basis_multiplier;

	/*
	 *	Data
	 */

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > PsqrtW(_P.data() + target_index*max_matrix_dimension*max_matrix_dimension, max_matrix_dimension, max_matrix_dimension);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > w(_w.data() + target_index*max_num_rows);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > V(_V.data() + target_index*_dimensions*_dimensions, _dimensions, _dimensions);
	scratch_matrix_type PTP(teamMember.team_scratch(_scratch_team_level_b), _dimensions, _dimensions);
	// delta is a temporary variable that preallocates space on which solutions of size _NP will
	// be computed. This allows the gpu to allocate the memory in advance
	scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), max_NP*_basis_multiplier);

	/*
	 *	Determine Coarse Approximation of Manifold Tangent Plane
	 */

	// getting x y and z from which to derive a manifold
	this->createWeightsAndPForCurvature(teamMember, delta, PsqrtW, w, _dimensions, true /* only specific order */);

	// create PsqrtW^T*PsqrtW
	GMLS_LinearAlgebra::createM(teamMember, PTP, PsqrtW, _dimensions /* # of columns */, this->getNNeighbors(target_index));

	// create coarse approximation of tangent plane in first two columns of V, with normal direction in third column
	GMLS_LinearAlgebra::largestTwoEigenvectorsThreeByThreeSymmetric(teamMember, V, PTP, _dimensions);

}

//KOKKOS_INLINE_FUNCTION
//void GMLS::operator()(const AssembleManifoldCurvaturePsqrtW&, const member_type& teamMember) const {
//}
//
//KOKKOS_INLINE_FUNCTION
//void GMLS::operator()(const ApplyCurvatureTargets&, const member_type& teamMember) const {
//}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const AssembleManifoldPsqrtW&, const member_type& teamMember) const {

	/*
	 *	Dimensions
	 */

	const int target_index = teamMember.league_rank();
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;

	// these dimensions already calculated differ in the case of manifolds
	const int max_num_rows = _sampling_multiplier*max_num_neighbors;
	const int manifold_NP = this->getNP(_manifold_poly_order, _dimensions-1);
	const int target_NP = this->getNP(_poly_order, _dimensions-1);
	const int max_manifold_NP = (manifold_NP > target_NP) ? manifold_NP : target_NP;
	const int max_NP = (max_manifold_NP > _NP) ? max_manifold_NP : _NP;
	const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
	const int this_num_columns = _basis_multiplier*max_manifold_NP;
	const int max_matrix_dimension = (max_num_neighbors*_sampling_multiplier > max_NP*_basis_multiplier)
		? max_num_neighbors*_sampling_multiplier : max_NP*_basis_multiplier;
	// max_num_rows remains the same
	const int max_P_row_size = ((_dimensions-1)*manifold_NP > max_NP*_total_alpha_values*_basis_multiplier) ? (_dimensions-1)*manifold_NP : max_NP*_total_alpha_values*_basis_multiplier;

	/*
	 *	Data
	 */

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > PsqrtW(_P.data() + target_index*max_matrix_dimension*max_matrix_dimension, max_matrix_dimension, max_matrix_dimension);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > Q(_RHS.data() + target_index*max_num_rows*max_num_rows, max_num_rows, max_num_rows);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > w(_w.data() + target_index*max_num_rows);

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > V(_V.data() + target_index*_dimensions*_dimensions, _dimensions, _dimensions);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > T(_T.data() + target_index*_dimensions*(_dimensions-1), _dimensions, _dimensions-1);

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > G_inv(_manifold_metric_tensor_inverse.data() + target_index*(_dimensions-1)*(_dimensions-1), _dimensions-1, _dimensions-1);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > manifold_coeffs(_manifold_curvature_coefficients.data() + target_index*manifold_NP);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > manifold_gradient_coeffs(_manifold_curvature_gradient.data() + target_index*(_dimensions-1));

	scratch_matrix_type G(teamMember.team_scratch(_scratch_team_level_b), _dimensions-1, _dimensions-1);

	scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_b), max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
	scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_b), max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
	scratch_vector_type t3(teamMember.team_scratch(_scratch_team_level_b), 1);

	scratch_vector_type manifold_gradient(teamMember.team_scratch(_scratch_team_level_b), (_dimensions-1)*max_num_neighbors);
	scratch_matrix_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), max_P_row_size, _sampling_multiplier);

	// delta is a temporary variable that preallocates space on which solutions of size _NP will
	// be computed. This allows the gpu to allocate the memory in advance
	scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), max_NP*_basis_multiplier);

	scratch_vector_type S(teamMember.team_scratch(_scratch_team_level_b), max_manifold_NP*_basis_multiplier);
	scratch_matrix_type b_data(teamMember.team_scratch(_scratch_team_level_b), max_manifold_NP*_basis_multiplier, _sampling_multiplier);
	scratch_matrix_type Vsvd(teamMember.team_scratch(_scratch_team_level_b), max_manifold_NP*_basis_multiplier, max_manifold_NP*_basis_multiplier);


	/*
	 *	Manifold
	 */


	//
	//  RECONSTRUCT ON THE TANGENT PLANE USING LOCAL COORDINATES
	//

	// creates the matrix sqrt(W)*P
	this->createWeightsAndPForCurvature(teamMember, delta, PsqrtW, w, _dimensions-1, false /* only specific order */, &V);

	GMLS_LinearAlgebra::GivensQR(teamMember, t1, t2, t2, Q, PsqrtW, manifold_NP, this->getNNeighbors(target_index));
	teamMember.team_barrier();

	// gives GMLS coefficients for gradient using basis defined on reduced space
	GMLS_LinearAlgebra::upperTriangularBackSolve(teamMember, t1, t2, Q, PsqrtW, w, manifold_NP, this->getNNeighbors(target_index)); // stores solution in Q
	teamMember.team_barrier();

	//
	//  GET TARGET COEFFICIENTS RELATED TO GRADIENT TERMS
	//
	// reconstruct grad_xi1 and grad_xi2, not used for manifold_coeffs
	this->computeManifoldFunctionals(teamMember, t1, t2, P_target_row, &V, -1 /*target as neighbor index*/, 1 /*alpha*/);

	Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
		for (int j=0; j<manifold_NP; ++j) { // set to zero
			manifold_coeffs(j) = 0;
		}
	});

	double grad_xi1 = 0, grad_xi2 = 0;
	for (int i=0; i<this->getNNeighbors(target_index); ++i) {
		for (int k=0; k<_dimensions-1; ++k) {
			double alpha_ij = 0;
			Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember,
					manifold_NP), [=] (const int l, double &talpha_ij) {
				talpha_ij += P_target_row(manifold_NP*k+l,0)*Q(i,l);
			}, alpha_ij);
			Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
				manifold_gradient(i*(_dimensions-1) + k) = alpha_ij; // stored staggered, grad_xi1, grad_xi2, grad_xi1, grad_xi2, ....
			});
		}
		teamMember.team_barrier();

		XYZ rel_coord = getRelativeCoord(target_index, i, _dimensions, &V);
		double normal_coordinate = rel_coord[_dimensions-1];

		// apply coefficients to sample data
		grad_xi1 += manifold_gradient(i*(_dimensions-1)) * normal_coordinate;
		if (_dimensions>2) grad_xi2 += manifold_gradient(i*(_dimensions-1)+1) * normal_coordinate;

		Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {
			// coefficients without a target premultiplied
			for (int j=0; j<manifold_NP; ++j) {
				manifold_coeffs(j) += Q(i,j) * normal_coordinate;
			}
		});
	}

	// Constructs high order orthonormal tangent space T and inverse of metric tensor
	Kokkos::single(Kokkos::PerTeam(teamMember), [&] () {

		manifold_gradient_coeffs(0) = grad_xi1;
		if (_dimensions>2) manifold_gradient_coeffs(1) = grad_xi2;

		// Construct T (high order approximation of orthonormal tangent vectors)
		for (int i=0; i<_dimensions-1; ++i) {
			// build
			for (int j=0; j<_dimensions; ++j) {
				T(j,i) = manifold_gradient_coeffs(i)*V(j,_dimensions-1);
				T(j,i) += V(j,i);
			}
		}

		// calculate norm
		double norm = 0;
		for (int j=0; j<_dimensions; ++j) {
			norm += T(j,0)*T(j,0);
		}

		// normalize first vector
		norm = std::sqrt(norm);
		for (int j=0; j<_dimensions; ++j) {
			T(j,0) /= norm;
		}

		// orthogonalize next vector
		if (_dimensions-1 == 2) { // 2d manifold
			double dot_product = T(0,0)*T(0,1) + T(1,0)*T(1,1) + T(2,0)*T(2,1);
			for (int j=0; j<_dimensions; ++j) {
				T(j,1) -= dot_product*T(j,0);
			}
			// normalize second vector
			norm = 0;
			for (int j=0; j<_dimensions; ++j) {
				norm += T(j,1)*T(j,1);
			}
			norm = std::sqrt(norm);
			for (int j=0; j<_dimensions; ++j) {
				T(j,1) /= norm;
			}
		}

		// need to get 2x2 matrix of metric tensor
		G(0,0) = 1 + grad_xi1*grad_xi1;

		if (_dimensions>2) {
			G(0,1) = grad_xi1*grad_xi2;
			G(1,0) = grad_xi2*grad_xi1;
			G(1,1) = 1 + grad_xi2*grad_xi2;
		}

		double G_determinant;
		if (_dimensions==2) {
			G_determinant = G(0,0);
			ASSERT_WITH_MESSAGE(G_determinant!=0, "Determinant is zero.");
			G_inv(0,0) = 1/G_determinant;
		} else {
			G_determinant = G(0,0)*G(1,1) - G(0,1)*G(1,0); //std::sqrt(G_inv(0,0)*G_inv(1,1) - G_inv(0,1)*G_inv(1,0));
			ASSERT_WITH_MESSAGE(G_determinant!=0, "Determinant is zero.");
			{
				// inverse of 2x2
				G_inv(0,0) = G(1,1)/G_determinant;
				G_inv(1,1) = G(0,0)/G_determinant;
				G_inv(0,1) = -G(0,1)/G_determinant;
				G_inv(1,0) = -G(1,0)/G_determinant;
			}
		}

	});
	teamMember.team_barrier();
	//
	//  END OF MANIFOLD METRIC CALCULATIONS
	//


	// PsqrtW contains valid entries if _poly_order == _manifold_poly_order
	// However, if they differ, then they must be recomputed
	this->createWeightsAndP(teamMember, delta, PsqrtW, w, _dimensions-1, _poly_order, true /* weight with W*/, &V, &T, _polynomial_sampling_functional);
	teamMember.team_barrier();

	// fill in RHS with Identity * sqrt(weights)
	Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this_num_rows,max_num_rows), [=] (const int i) {
		for(int j = 0; j < this_num_columns; ++j) {
			PsqrtW(i,j) = 0;
		}
	});
//if (!_nontrivial_nullspace) {
#if not(defined(COMPADRE_USE_CUDA))
	// LAPACK calls use Fortran data layout so we need LayoutLeft but have LayoutRight
	GMLS_LinearAlgebra::matrixToLayoutLeft(teamMember, t1, t2, PsqrtW, this_num_columns, this_num_rows);
#endif
	
	// fill in RHS with Identity * sqrt(weights)
	Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,max_num_rows), [=] (const int i) {
		for(int j = 0; j < max_num_rows; ++j) {
			if (i < this_num_rows) 
				Q(ORDER_INDICES(j,i)) = (i==j) ? std::sqrt(w(i)) : 0;
			else
				Q(ORDER_INDICES(j,i)) = 0;
		}
	});
//}

       teamMember.team_barrier();
}

KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ApplyManifoldTargets&, const member_type& teamMember) const {

	/*
	 *	Dimensions
	 */

	const int target_index = teamMember.league_rank();
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;

	// these dimensions already calculated differ in the case of manifolds
	const int max_num_rows = _sampling_multiplier*max_num_neighbors;
	const int manifold_NP = this->getNP(_manifold_poly_order, _dimensions-1);
	const int target_NP = this->getNP(_poly_order, _dimensions-1);
	const int max_manifold_NP = (manifold_NP > target_NP) ? manifold_NP : target_NP;
	const int max_NP = (max_manifold_NP > _NP) ? max_manifold_NP : _NP;
	const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
	const int this_num_columns = _basis_multiplier*max_manifold_NP;
	const int max_matrix_dimension = (max_num_neighbors*_sampling_multiplier > max_NP*_basis_multiplier)
		? max_num_neighbors*_sampling_multiplier : max_NP*_basis_multiplier;
	// max_num_rows remains the same
	const int max_P_row_size = ((_dimensions-1)*manifold_NP > max_NP*_total_alpha_values*_basis_multiplier) ? (_dimensions-1)*manifold_NP : max_NP*_total_alpha_values*_basis_multiplier;

	/*
	 *	Data
	 */

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > PsqrtW(_P.data() + target_index*max_matrix_dimension*max_matrix_dimension, max_matrix_dimension, max_matrix_dimension);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > Q(_RHS.data() + target_index*max_num_rows*max_num_rows, max_num_rows, max_num_rows);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > w(_w.data() + target_index*max_num_rows);

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > V(_V.data() + target_index*_dimensions*_dimensions, _dimensions, _dimensions);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > T(_T.data() + target_index*_dimensions*(_dimensions-1), _dimensions, _dimensions-1);

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > G_inv(_manifold_metric_tensor_inverse.data() + target_index*(_dimensions-1)*(_dimensions-1), _dimensions-1, _dimensions-1);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > manifold_coeffs(_manifold_curvature_coefficients.data() + target_index*manifold_NP);
	Kokkos::View<double*, Kokkos::MemoryTraits<Kokkos::Unmanaged> > manifold_gradient_coeffs(_manifold_curvature_gradient.data() + target_index*(_dimensions-1));

	scratch_matrix_type G(teamMember.team_scratch(_scratch_team_level_b), _dimensions-1, _dimensions-1);

	scratch_vector_type t1(teamMember.team_scratch(_scratch_team_level_b), max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
	scratch_vector_type t2(teamMember.team_scratch(_scratch_team_level_b), max_num_neighbors*((_sampling_multiplier>_basis_multiplier) ? _sampling_multiplier : _basis_multiplier));
	scratch_vector_type t3(teamMember.team_scratch(_scratch_team_level_b), 1);

	scratch_vector_type manifold_gradient(teamMember.team_scratch(_scratch_team_level_b), (_dimensions-1)*max_num_neighbors);
	scratch_matrix_type P_target_row(teamMember.team_scratch(_scratch_team_level_b), max_P_row_size, _sampling_multiplier);

	// delta is a temporary variable that preallocates space on which solutions of size _NP will
	// be computed. This allows the gpu to allocate the memory in advance
	scratch_vector_type delta(teamMember.thread_scratch(_scratch_thread_level_b), max_NP*_basis_multiplier);

	scratch_vector_type S(teamMember.team_scratch(_scratch_team_level_b), max_manifold_NP*_basis_multiplier);
	scratch_matrix_type b_data(teamMember.team_scratch(_scratch_team_level_b), max_manifold_NP*_basis_multiplier, _sampling_multiplier);
	scratch_matrix_type Vsvd(teamMember.team_scratch(_scratch_team_level_b), max_manifold_NP*_basis_multiplier, max_manifold_NP*_basis_multiplier);

	/*
	 *	Apply Standard Target Evaluations to Polynomial Coefficients
	 */

	for (int n=0; n<_sampling_multiplier; ++n) {
		this->computeTargetFunctionalsOnManifold(teamMember, t1, t2, P_target_row, _operations, V, T, G_inv, manifold_coeffs, manifold_gradient_coeffs, n);
	}
	teamMember.team_barrier();

	//if (_nontrivial_nullspace) {

	//	GMLS_LinearAlgebra::GolubReinschSVD(teamMember, t1, t2, PsqrtW, Q, S, Vsvd, target_NP*_basis_multiplier /* custom # of columns*/, this->getNNeighbors(target_index)*_sampling_multiplier /* custom # of rows*/);

	//	// threshold for dropping singular values
	//	double S0 = S(0);
	//	double eps = 1e-14;
	//	int maxVal = (target_NP*_basis_multiplier > _sampling_multiplier*this->getNNeighbors(target_index)) ? target_NP*_basis_multiplier : this->getNNeighbors(target_index)*_sampling_multiplier;
	//	double abs_threshold = maxVal*eps*S0;

	//	this->applySVD(teamMember, b_data, t1, Q, S, Vsvd, w, P_target_row, target_NP, abs_threshold);

	//} else {
//if (_nontrivial_nullspace) {
//#if not(defined(COMPADRE_USE_CUDA))
//	// LAPACK calls use Fortran data layout so we need LayoutLeft but have LayoutRight
//	GMLS_LinearAlgebra::matrixToLayoutLeft(teamMember, t1, t2, Q, this_num_rows, this_num_rows);
//#endif
//}

		this->applyQR(teamMember, t1, t2, Q, PsqrtW, w, P_target_row, target_NP); 
	//}
	teamMember.team_barrier();
}


KOKKOS_INLINE_FUNCTION
void GMLS::operator()(const ComputePrestencilWeights&, const member_type& teamMember) const {

	/*
	 *	Dimensions
	 */

	const int target_index = teamMember.league_rank();
	const int max_num_neighbors = _neighbor_lists.dimension_1()-1;

	// these dimensions already calculated differ in the case of manifolds
	const int max_num_rows = _sampling_multiplier*max_num_neighbors;
	const int manifold_NP = this->getNP(_manifold_poly_order, _dimensions-1);
	const int target_NP = this->getNP(_poly_order, _dimensions-1);
	const int max_manifold_NP = (manifold_NP > target_NP) ? manifold_NP : target_NP;
	const int max_NP = (max_manifold_NP > _NP) ? max_manifold_NP : _NP;
	const int this_num_rows = _sampling_multiplier*this->getNNeighbors(target_index);
	const int this_num_columns = _basis_multiplier*max_manifold_NP;
	const int max_matrix_dimension = (max_num_neighbors*_sampling_multiplier > max_NP*_basis_multiplier)
		? max_num_neighbors*_sampling_multiplier : max_NP*_basis_multiplier;
	// max_num_rows remains the same
	const int max_P_row_size = ((_dimensions-1)*manifold_NP > max_NP*_total_alpha_values*_basis_multiplier) ? (_dimensions-1)*manifold_NP : max_NP*_total_alpha_values*_basis_multiplier;

	/*
	 *	Data
	 */

	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > V(_V.data() + target_index*_dimensions*_dimensions, _dimensions, _dimensions);
	Kokkos::View<double**, layout_type, Kokkos::MemoryTraits<Kokkos::Unmanaged> > T(_T.data() + target_index*_dimensions*(_dimensions-1), _dimensions, _dimensions-1);


	/*
	 *	Prestencil Weight Calculations
	 */

	if (_data_sampling_functional == ReconstructionOperator::SamplingFunctional::StaggeredEdgeAnalyticGradientIntegralSample) {

		double operator_coefficient = 1;
		if (_operator_coefficients.dimension_0() > 0) {
			operator_coefficient = _operator_coefficients(this->getNeighborIndex(target_index,0),0);
		}
		_prestencil_weights(target_index, 0) = -operator_coefficient;
		_prestencil_weights(target_index, 1) =  operator_coefficient;

		for (int i=1; i<this->getNNeighbors(target_index); ++i) {

			if (_operator_coefficients.dimension_0() > 0) {
				operator_coefficient = 0.5*(_operator_coefficients(this->getNeighborIndex(target_index,0),0) + _operator_coefficients(this->getNeighborIndex(target_index,i),0));
			}
			_prestencil_weights(target_index, 2*i  ) = -operator_coefficient;
			_prestencil_weights(target_index, 2*i+1) =  operator_coefficient;

		}
	}
	// generate prestencils that require information about the change of basis to the manifold
	else if (_data_sampling_functional == ReconstructionOperator::SamplingFunctional::ManifoldVectorSample) {
		const int neighbor_offset = _neighbor_lists.dimension_1()-1;
		Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this->getNNeighbors(target_index)),
					[=] (const int m) {
			for (int j=0; j<_dimensions; ++j) {
				for (int k=0; k<_dimensions-1; ++k) {
					_prestencil_weights(target_index, k*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m  ) =  T(j,k);
					_prestencil_weights(target_index, k*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m+1) =  0;
				}
				_prestencil_weights(target_index, (_dimensions-1)*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m  ) =  0;
				_prestencil_weights(target_index, (_dimensions-1)*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m+1) =  0;
			}
		});
	} else if (_data_sampling_functional == ReconstructionOperator::SamplingFunctional::ManifoldGradientVectorSample) {
		const int neighbor_offset = _neighbor_lists.dimension_1()-1;
		Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this->getNNeighbors(target_index)),
					[=] (const int m) {
			for (int j=0; j<_dimensions; ++j) {
				for (int k=0; k<_dimensions-1; ++k) {
					_prestencil_weights(target_index, k*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m  ) =  V(j,k);
					_prestencil_weights(target_index, k*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m+1) =  0;
				}
				_prestencil_weights(target_index, (_dimensions-1)*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m  ) =  0;
				_prestencil_weights(target_index, (_dimensions-1)*_dimensions*2*neighbor_offset + j*2*neighbor_offset + 2*m+1) =  0;
			}
		});
	} else if (_data_sampling_functional == ReconstructionOperator::SamplingFunctional::StaggeredEdgeIntegralSample) {
		const int neighbor_offset = _neighbor_lists.dimension_1()-1;
		Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,this->getNNeighbors(target_index)), [=] (const int m) {
			for (int quadrature = 0; quadrature<_number_of_quadrature_points; ++quadrature) {
				XYZ tangent_quadrature_coord_2d;
				for (int j=0; j<_dimensions-1; ++j) {
					tangent_quadrature_coord_2d[j] = getTargetCoordinate(target_index, j, &V);
					tangent_quadrature_coord_2d[j] -= getNeighborCoordinate(target_index, m, j, &V);
				}
				double tangent_vector[3];
				tangent_vector[0] = tangent_quadrature_coord_2d[0]*V(0,0) + tangent_quadrature_coord_2d[1]*V(0,1);
				tangent_vector[1] = tangent_quadrature_coord_2d[0]*V(1,0) + tangent_quadrature_coord_2d[1]*V(1,1);
				tangent_vector[2] = tangent_quadrature_coord_2d[0]*V(2,0) + tangent_quadrature_coord_2d[1]*V(2,1);

				for (int j=0; j<_dimensions; ++j) {
					_prestencil_weights(target_index, j*2*neighbor_offset + 2*m  ) +=  (1-_parameterized_quadrature_sites[quadrature])*tangent_vector[j]*_quadrature_weights[quadrature];
					_prestencil_weights(target_index, j*2*neighbor_offset + 2*m+1) +=  _parameterized_quadrature_sites[quadrature]*tangent_vector[j]*_quadrature_weights[quadrature];
				}
			}
		});
	}
}
#endif

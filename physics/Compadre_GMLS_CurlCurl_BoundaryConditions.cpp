#include <Compadre_GMLS_CurlCurl_BoundaryConditions.hpp>

#include <Compadre_CoordsT.hpp>
#include <Compadre_ParticlesT.hpp>
#include <Compadre_FieldT.hpp>
#include <Compadre_FieldManager.hpp>
#include <Compadre_DOFManager.hpp>
#include <Compadre_XyzVector.hpp>
#include <Compadre_AnalyticFunctions.hpp>

namespace Compadre {

typedef Compadre::FieldT fields_type;
typedef Compadre::XyzVector xyz_type;

void GMLS_CurlCurlBoundaryConditions::flagBoundaries() {
	device_view_type pts = this->_coords->getPts()->getLocalView<device_view_type>();
	local_index_type bc_id_size = this->_particles->getFlags()->getLocalLength();
	Kokkos::parallel_for(Kokkos::RangePolicy<Kokkos::DefaultHostExecutionSpace>(0,bc_id_size), KOKKOS_LAMBDA(const int i) {
		scalar_type epsilon_ball = 1e-6;
		if (std::abs(pts(i,0)-1.0)<epsilon_ball || std::abs(pts(i,0)+1.0)<epsilon_ball || std::abs(pts(i,1)-1.0)<epsilon_ball || std::abs(pts(i,1)+1.0)<epsilon_ball || std::abs(pts(i,2)-1.0)<epsilon_ball || std::abs(pts(i,2)+1.0)<epsilon_ball) {
                    // std::cout << "BC particles " << i << " x " << pts(i, 0) << " y " << pts(i, 1) << " z " << pts(i, 2) << std::endl;
                    this->_particles->setFlag(i, 1);
		} else {
                    this->_particles->setFlag(i, 0);
		}
	});

}

void GMLS_CurlCurlBoundaryConditions::applyBoundaries(local_index_type field_one, local_index_type field_two, scalar_type time, scalar_type current_timestep_size, scalar_type previous_timestep_size) {
	Teuchos::RCP<Compadre::AnalyticFunction> function;
	if (_parameters->get<std::string>("solution type")=="sine") {
		function = Teuchos::rcp_static_cast<Compadre::AnalyticFunction>(Teuchos::rcp(new Compadre::CurlCurlSineTest));
	} else {
		function = Teuchos::rcp_static_cast<Compadre::AnalyticFunction>(Teuchos::rcp(new Compadre::CurlCurlPolyTest));
	}

	TEUCHOS_TEST_FOR_EXCEPT_MSG(_b==NULL, "Tpetra Multivector for BCS not yet specified.");
	if (field_two == -1) {
		field_two = field_one;
	}

	host_view_local_index_type bc_id = this->_particles->getFlags()->getLocalView<host_view_local_index_type>();
	host_view_type rhs_vals = this->_b->getLocalView<host_view_type>();
	host_view_type pts = this->_coords->getPts()->getLocalView<host_view_type>();


	const local_index_type nlocal = static_cast<local_index_type>(this->_coords->nLocal());
	const std::vector<Teuchos::RCP<fields_type> >& fields = this->_particles->getFieldManagerConst()->getVectorOfFields();
	const local_dof_map_view_type local_to_dof_map = _dof_data->getDOFMap();

	for (local_index_type i=0; i<nlocal; ++i) { // parallel_for causes cache thrashing
		// get dof corresponding to field
		for (local_index_type k = 0; k < fields[field_one]->nDim(); ++k) {
			const local_index_type dof = local_to_dof_map(i, field_one, k);
			xyz_type pt(pts(i, 0), pts(i, 1), pts(i, 2));
			if (bc_id(i,0)==1) rhs_vals(dof,0) = function->evalVector(pt)[k];
		}
	}


}

std::vector<InteractingFields> GMLS_CurlCurlBoundaryConditions::gatherFieldInteractions() {
	std::vector<InteractingFields> field_interactions;
        field_interactions.push_back(InteractingFields(op_needing_interaction::bc, _particles->getFieldManagerConst()->getIDOfFieldFromName("vector solution")));
	return field_interactions;
}

}

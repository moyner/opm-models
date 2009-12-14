// $Id$
/*****************************************************************************
 *   Copyright (C) 2007 by Peter Bastian                                     *
 *   Institute of Parallel and Distributed System                            *
 *   Department Simulation of Large Systems                                  *
 *   University of Stuttgart, Germany                                        *
 *                                                                           *
 *   Copyright (C) 2008 by Andreas Lauser, Bernd Flemisch                    *
 *   Institute of Hydraulic Engineering                                      *
 *   University of Stuttgart, Germany                                        *
 *   email: <givenname>.<name>@iws.uni-stuttgart.de                          *
 *                                                                           *
 *   This program is free software; you can redistribute it and/or modify    *
 *   it under the terms of the GNU General Public License as published by    *
 *   the Free Software Foundation; either version 2 of the License, or       *
 *   (at your option) any later version, as long as this copyright notice    *
 *   is included in its original form.                                       *
 *                                                                           *
 *   This program is distributed WITHOUT ANY WARRANTY.                       *
 *****************************************************************************/
#ifndef DUMUX_BOX_SCHEME_HH
#define DUMUX_BOX_SCHEME_HH

#include <dumux/boxmodels/boxscheme/boxjacobian.hh>
#include <dumux/auxiliary/valgrind.hh>

#include <dune/istl/operators.hh>
#include <dune/disc/operators/p1operator.hh>
#include <dune/grid/common/genericreferenceelements.hh>
//#include <dune/grid/common/referenceelements.hh>

#include <boost/format.hpp>

#include "boxproperties.hh"

namespace Dune
{

/*!
 * \defgroup BoxScheme   Box-Scheme
 */
/*!
 * \ingroup BoxScheme
 * \defgroup BoxProblems Box-Problems
 */
/*!
 * \ingroup BoxScheme
 * \defgroup BoxModels   Box-Models
 */


/*!
 * \ingroup BoxScheme
 *
 * \brief The base class for the vertex centered finite volume
 *        discretization scheme.
 */
template<class TypeTag, class Implementation>
class BoxScheme
{
    typedef BoxScheme<TypeTag, Implementation>               ThisType;
    typedef typename GET_PROP_TYPE(TypeTag, PTAG(Problem))   Problem;
    typedef typename GET_PROP_TYPE(TypeTag, PTAG(GridView))  GridView;

    typedef typename GET_PROP_TYPE(TypeTag, PTAG(Scalar))    Scalar;
    typedef typename GridView::Grid::ctype                   CoordScalar;

    typedef typename GET_PROP(TypeTag, PTAG(SolutionTypes)) SolutionTypes;
    typedef typename SolutionTypes::SolutionFunction        SolutionFunction;
    typedef typename SolutionTypes::DofEntityMapper         DofEntityMapper;
    typedef typename SolutionTypes::VertexMapper            VertexMapper;
    typedef typename SolutionTypes::ElementMapper           ElementMapper;
    typedef typename SolutionTypes::Solution                Solution;
    typedef typename SolutionTypes::SolutionOnElement       SolutionOnElement;
    typedef typename SolutionTypes::PrimaryVarVector        PrimaryVarVector;
    typedef typename SolutionTypes::BoundaryTypeVector      BoundaryTypeVector;
    typedef typename SolutionTypes::ShapeFunctions          ShapeFunctions;
    typedef typename SolutionTypes::ShapeFunctionSet        ShapeFunctionSet;
    typedef typename SolutionTypes::JacobianAssembler       JacobianAssembler;

    enum {
        numEq   = GET_PROP_VALUE(TypeTag, PTAG(NumEq)),
        dim     = GridView::dimension
    };

    typedef typename GET_PROP(TypeTag, PTAG(ReferenceElements))        RefElemProp;
    typedef typename RefElemProp::Container                            ReferenceElements;
    typedef typename RefElemProp::ReferenceElement                     ReferenceElement;

    typedef typename GET_PROP_TYPE(TypeTag, PTAG(FVElementGeometry))   FVElementGeometry;

    typedef typename GET_PROP_TYPE(TypeTag, PTAG(LocalJacobian))     LocalJacobian;
    typedef typename GET_PROP_TYPE(TypeTag, PTAG(VertexData))        VertexData;

    typedef typename GET_PROP_TYPE(TypeTag, PTAG(NewtonMethod))      NewtonMethod;
    typedef typename GET_PROP_TYPE(TypeTag, PTAG(NewtonController))  NewtonController;

    typedef typename GridView::template Codim<0>::Entity               Element;
    typedef typename GridView::template Codim<0>::Iterator             ElementIterator;
    typedef typename GridView::IntersectionIterator                    IntersectionIterator;
    typedef typename GridView::template Codim<dim>::Entity             Vertex;
    typedef typename GridView::template Codim<dim>::Iterator           VertexIterator;

public:
    /*!
     *  \brief This structure is Required to use models based on the BOX
     *         scheme in conjunction with the newton Method.
     *
     * \todo change the newton method to the property system!
     */
    struct NewtonTraits {
        typedef typename ThisType::LocalJacobian     LocalJacobian;
        typedef typename ThisType::SolutionFunction  Function;
        typedef typename ThisType::JacobianAssembler JacobianAssembler;
        typedef typename ThisType::Scalar            Scalar;
        typedef typename ThisType::GridView::Grid    Grid;
    };

    /*!
     * \brief The constructor.
     */
    BoxScheme(Problem &prob)
        : problem_(prob),
          gridView_(prob.gridView()),
          dofEntityMapper_(gridView_),
          vertexMapper_(gridView_),
          elementMapper_(gridView_),

          localJacobian_(prob)
    {
        wasRestarted_ = false;

        jacAsm_ = 0;
        uCur_ = 0;
        uPrev_ = 0;
        f_ = 0;

        // check grid partitioning if we are parallel
        assert((prob.gridView().comm().size() == 1) ||
               (prob.gridView().overlapSize(0) > 0) ||
               (prob.gridView().ghostSize(0) > 0));
    }

    ~BoxScheme()
    { 
        delete jacAsm_;
        delete uCur_;
        delete uPrev_;
        delete f_;
    }


    /*!
     * \brief Apply the initial conditions to the model.
     */
    void initial()
    {
    	if (!wasRestarted_) {
            allocateStuff_();
            this->localJacobian().initStaticData();
            applyInitialSolution_(*uCur_);
        }

        applyDirichletBoundaries_(*uCur_);

        // also set the solution of the "previous" time step to the
        // initial solution.
        *(*uPrev_) = *(*uCur_);

        // update the static vertex data with the initial solution
        this->localJacobian().updateStaticData(*uCur_, *uPrev_);
    }

    Scalar globalResidual(const SolutionFunction &u, SolutionFunction &tmp) 
    {
        SolutionFunction tmpU(gridView_, gridView_);
        *tmpU = *uCur_;
        *uCur_ = *u;
        localJacobian_.evalGlobalResidual(tmp);

        Scalar result = (*tmp).two_norm();
        /*
        Scalar result = 0;
        for (int i = 0; i < (*tmp).size(); ++i) {
            for (int j = 0; j < numEq; ++j)
                result += std::abs((*tmp)[i][j]);
        }
        */
        *uCur_ = *tmpU;
        return result;
    };
    /*!
     * \brief Reference to the current solution function.
     */
    const SolutionFunction &curSolFunction() const
    { return *uCur_; }

    /*!
     * \brief Reference to the current solution function.
     */
    SolutionFunction &curSolFunction()
    { return *uCur_; }

    /*!
     * \brief Reference to the solution function for the right hand side.
     */
    SolutionFunction &rightHandSideFunction()
    { return *f_; }

    /*!
     * \brief Reference to solution function of the previous time step.
     */
    SolutionFunction &prevSolFunction()
    { return *uPrev_; }

    /*!
     * \brief Reference to solution function of the previous time step.
     */
    const SolutionFunction &prevSolFunction() const
    { return *uPrev_; }

    /*!
     * \brief Returns the operator assembler for the global jacobian of
     *        the problem.
     */
    JacobianAssembler &jacobianAssembler()
    { return *jacAsm_; }

    /*!
     * \brief Returns the local jacobian which calculates the local
     *        stiffness matrix for an arbitrary element.
     *
     * The local stiffness matrices of the element are used by
     * the jacobian assembler to produce a global linerization of the
     * problem.
     */
    LocalJacobian &localJacobian()
    { return localJacobian_; }
    /*!
     * \copydoc localJacobian()
     */
    const LocalJacobian &localJacobian() const
    { return localJacobian_; }


    /*!
     * \brief A reference to the problem on which the model is applied.
     */
    Problem &problem()
    { return problem_; }
    /*!
     * \copydoc problem()
     */
    const Problem &problem() const
    { return problem_; }

    /*!
     * \brief Reference to the grid view of the spatial domain.
     */
    const GridView &gridView() const
    { return gridView_; }

    /*!
     * \brief Try to progress the model to the next timestep.
     */
    void update(Scalar &dt,
                Scalar &nextDt,
                NewtonMethod &solver,
                NewtonController &controller)
    {
#if HAVE_VALGRIND
        for (size_t i = 0; i < (*curSolFunction()).size(); ++i)
            Valgrind::CheckDefined((*curSolFunction())[i]);
#endif // HAVE_VALGRIND

        asImp_().updateBegin();

        int numRetries = 0;
        while (true)
        {
            bool converged = solver.execute(this->asImp_(),
                                            controller);
            nextDt = controller.suggestTimeStepSize(dt);
            if (converged) {
                std::cout << boost::format("Newton solver converged for rank %d\n")
                    %gridView_.comm().rank();
                break;
            }

            ++numRetries;
            if (numRetries > 10)
                DUNE_THROW(Dune::MathError,
                           "Newton solver didn't converge after 10 timestep divisions. dt=" << dt);

            problem_.setTimeStepSize(nextDt);
            dt = nextDt;

            asImp_().updateFailedTry();

            std::cout << boost::format("Newton didn't converge for rank %d. Retrying with timestep of %f\n")
                %gridView_.comm().rank()%dt;
        }

        asImp_().updateSuccessful();

#if HAVE_VALGRIND
        for (size_t i = 0; i < (*curSolFunction()).size(); ++i) {
            Valgrind::CheckDefined(&(*curSolFunction())[i]);
        }
#endif // HAVE_VALGRIND
    }


    /*!
     * \brief Called by the update() method before it tries to
     *        apply the newton method. This is primary a hook
     *        which the actual model can overload.
     */
    void updateBegin()
    {
        applyDirichletBoundaries_(*uCur_);
    }


    /*!
     * \brief Called by the update() method if it was
     *        successful. This is primary a hook which the actual
     *        model can overload.
     */
    void updateSuccessful()
    {
        // make the current solution the previous one.
        *(*uPrev_) = *(*uCur_);
    };

    /*!
     * \brief Called by the update() method if a try was
     *         unsuccessful. This is primary a hook which the
     *         actual model can overload.
     */
    void updateFailedTry()
    {
        // Reset the current solution to the one of the
        // previous time step so that we can start the next
        // update at a physically meaningful solution.
        *(*uCur_) = *(*uPrev_);
        applyDirichletBoundaries_(*uCur_);
    };

    /*!
     * \brief Calculate the global residual.
     *
     * The global deflection of the mass balance from zero.
     */
    void evalGlobalResidual(SolutionFunction &globResidual)
    {
        (*globResidual) = Scalar(0.0);

        // iterate through leaf grid
        ElementIterator        it     = gridView_.template begin<0>();
        const ElementIterator &eendit = gridView_.template end<0>();
        for (; it != eendit; ++it)
        {
            // tell the local jacobian which element it should
            // consider and evaluate the local residual for the
            // element. in order to do this we first have to
            // evaluate the element's local solutions for the
            // current and the last timestep.
            const Element& element = *it;

            //const ShapeFunctionSet &sfs = ShapeFunctions::general(element.geometry().type(),
            //                                                      2 /* order */ );
            const int numDofs = 4;//sfs.size();
            SolutionOnElement localResidual(numDofs);

            SolutionOnElement localU(numDofs);
            SolutionOnElement localOldU(numDofs);

            localJacobian_.setCurrentElement(element);
            localJacobian_.restrictToElement(localU, curSolFunction());
            localJacobian_.restrictToElement(localOldU, prevSolFunction());

            localJacobian_.setCurrentSolution(localU);
            localJacobian_.setPreviousSolution(localOldU);

            localJacobian_.evalLocalResidual(localResidual);

            // loop over the element's shape functions, map their
            // associated degree of freedom to the corresponding
            // indices in the solution vector and add the element's
            // local residual at the index to the global residual at
            // this index.
            for(int dofIdx=0; dofIdx < numDofs; dofIdx++)
            {
                int globalIdx = dofEntityMapper().map(element, dofIdx, dim);
                                                      //sfs[dofIdx].entity(),
                                                      //sfs[dofIdx].codim());
                (*globResidual)[globalIdx] += localResidual[dofIdx];
            }
        }
    }

    /*!
     * \brief Serializes the current state of the model.
     */
    template <class Restarter>
    void serialize(Restarter &res)
    { res.template serializeEntities<dim>(asImp_(), this->gridView_); }

    /*!
     * \brief Deserializes the state of the model.
     */
    template <class Restarter>
    void deserialize(Restarter &res)
    {
      allocateStuff_();
        res.template deserializeEntities<dim>(asImp_(), this->gridView_);
        wasRestarted_ = true;
    }

    /*!
     * \brief Write the current solution for a vertex to a restart
     *        file.
     */
    void serializeEntity(std::ostream &outstream,
                         const Vertex &vert)
    {
        int vertIdx = dofEntityMapper().map(vert);

        // write phase state
        if (!outstream.good()) {
            DUNE_THROW(IOError,
                       "Could not serialize vertex "
                       << vertIdx);
        }

        for (int eqIdx = 0; eqIdx < numEq; ++eqIdx) {
            outstream << (*curSolFunction())[vertIdx][eqIdx] << " ";
        }
    };

    /*!
     * \brief Reads the current solution variables for a vertex from a
     *        restart file.
     */
    void deserializeEntity(std::istream &instream,
                           const Vertex &vert)
    {
        int vertIdx = dofEntityMapper().map(vert);
        for (int eqIdx = 0; eqIdx < numEq; ++eqIdx) {
            if (!instream.good())
                DUNE_THROW(IOError,
                           "Could not deserialize vertex "
                           << vertIdx);
            instream >> (*curSolFunction())[vertIdx][eqIdx];
        }
    };

    /*!
     * \brief Mapper for the entities where degrees of freedoms are
     *        defined to indices.
     *
     * This usually means a mapper for vertices.
     */
    const DofEntityMapper &dofEntityMapper() const
    { return dofEntityMapper_; };

    /*!
     * \brief Mapper for vertices to indices.
     */
    const VertexMapper &vertexMapper() const
    { return vertexMapper_; };

    /*!
     * \brief Mapper for elements to indices.
     */
    const ElementMapper &elementMapper() const
    { return elementMapper_; };


protected:
    //! returns true iff the grid has an overlap
    bool hasOverlap_()
    { return gridView_.overlapSize(0) > 0; };

  void allocateStuff_()
  {
#ifdef HAVE_DUNE_PDELAB
        jacAsm_ = new JacobianAssembler(asImp_(), problem_);
        uCur_ = new SolutionFunction(jacAsm_->gridFunctionSpace(), 0.0);
        uPrev_ = new SolutionFunction(jacAsm_->gridFunctionSpace(), 0.0);
        f_ = new SolutionFunction(jacAsm_->gridFunctionSpace(), 0.0);
#else
        jacAsm_ = new JacobianAssembler(gridView_.grid(), gridView_, gridView_, !hasOverlap_());
        uCur_ = new SolutionFunction(gridView_, gridView_, !hasOverlap_());
        uPrev_ = new SolutionFunction(gridView_, gridView_, !hasOverlap_());
        f_ = new SolutionFunction(gridView_, gridView_, !hasOverlap_());
#endif
  }

    void applyInitialSolution_(SolutionFunction &u)
    {
        // first set the whole domain to zero. This is
        // necessary in order to also get a meaningful value
        // for ghost nodes (if we are running in parallel)
        if (gridView_.comm().size() > 1) {
            (*u) = Scalar(0.0);
        }

        // iterate through leaf grid and evaluate initial
        // condition at the center of each sub control volume
        //
        // TODO: the initial condition needs to be unique for
        // each vertex. we should think about the API...
        ElementIterator it            = gridView_.template begin<0>();
        const ElementIterator &eendit = gridView_.template end<0>();
        for (; it != eendit; ++it)
        {
            // deal with the current element
            applyInitialSolutionElement_(u, *it);
        }
    };

    // apply the initial solition for a single element
    void applyInitialSolutionElement_(SolutionFunction &u,
                                      const Element &element)
    {
        // HACK: set the current element for the local
        // solution in order to get an updated FVElementGeometry
        localJacobian_.setCurrentElement(element);

        // loop over all element vertices, i.e. sub control volumes
        int numScv = element.template count<dim>();
        for (int scvIdx = 0;
             scvIdx < numScv;
             scvIdx++)
        {
            // map the local vertex index to the global one
            int globalIdx = dofEntityMapper().map(element,
                                                  scvIdx,
                                                  dim);

            const FVElementGeometry &fvElemGeom
                = localJacobian_.curFvElementGeometry();
            // use the problem for actually doing the
            // dirty work of nailing down the initial
            // solution.
            this->problem_.initial((*u)[globalIdx],
                                   element,
                                   fvElemGeom,
                                   scvIdx);
            Valgrind::CheckDefined((*u)[globalIdx]);
        }
    }

    // apply dirichlet boundaries for the whole grid
    void applyDirichletBoundaries_(SolutionFunction &u)
    {
        // set Dirichlet boundary conditions of the grid's
        // outer boundaries
        ElementIterator elementIt           = gridView_.template begin<0>();
        const ElementIterator &elementEndIt = gridView_.template end<0>();
        for (; elementIt != elementEndIt; ++elementIt)
        {
            // ignore elements which are not on the boundary of
            // the domain
            if (!elementIt->hasBoundaryIntersections())
                continue;

            // evaluate the element's boundary locally
            localJacobian_.updateBoundaryTypes(*elementIt);

            // apply dirichlet boundary for the current element
            applyDirichletElement_(u, *elementIt);
        }
    };

    // apply dirichlet boundaries for a single element
    void applyDirichletElement_(SolutionFunction &u,
                                const Element &element)
    {
        Dune::GeometryType      geoType = element.geometry().type();
        const ReferenceElement &refElem = ReferenceElements::general(geoType);

        // loop over all the element's surface patches
        IntersectionIterator isIt = gridView_.ibegin(element);
        const IntersectionIterator &isEndIt = gridView_.iend(element);
        for (; isIt != isEndIt; ++isIt) {
            // if the current intersection is not on the boundary,
            // we ignore it
            if (!isIt->boundary())
                continue;

            // Assemble the boundary for all vertices of the
            // current face
            int faceIdx = isIt->indexInInside();
            int numVerticesOfFace = refElem.size(faceIdx, 1, dim);
            for (int vertInFace = 0;
                 vertInFace < numVerticesOfFace;
                 vertInFace++)
            {
                // apply dirichlet boundaries for the current
                // sub-control volume face
                applyDirichletSCVF_(u, element, refElem, isIt, vertInFace);
            }
        }
    }

    // apply dirichlet boundaries for a single boundary
    // sub-control volume face of a finite volume cell.
    void applyDirichletSCVF_(SolutionFunction &u,
                             const Element &element,
                             const ReferenceElement &refElem,
                             const IntersectionIterator &isIt,
                             int faceVertIdx)
    {
        // apply dirichlet boundaries but make sure
        // not to interfere with non-dirichlet
        // boundaries...
        const FVElementGeometry &fvElemGeom =
            localJacobian_.curFvElementGeometry();

        int elemVertIdx = refElem.subEntity(isIt->indexInInside(), 1,
                                            faceVertIdx, dim);
        int boundaryFaceIdx = fvElemGeom.boundaryFaceIndex(isIt->indexInInside(),
                                                           faceVertIdx);
        int globalVertexIdx = dofEntityMapper().map(element,
                                                    elemVertIdx, dim);


        PrimaryVarVector dirichletVal;
        BoundaryTypeVector boundaryTypes;
        problem_.boundaryTypes(boundaryTypes,
                               element,
                               fvElemGeom,
                               *isIt,
                               elemVertIdx,
                               boundaryFaceIdx);

        bool dirichletEvaluated = false;
        for (int eqIdx = 0; eqIdx < numEq; ++eqIdx)
        {
            // ignore non-dirichlet boundary conditions
            if (!boundaryTypes.isDirichlet(eqIdx))
            { continue; }

            // make sure to evaluate the dirichlet boundary
            // conditions exactly once (and only if the boundary
            // type is actually dirichlet).
            if (!dirichletEvaluated)
            {
                dirichletEvaluated = true;
                problem_.dirichlet(dirichletVal,
                                   element,
                                   fvElemGeom,
                                   *isIt,
                                   elemVertIdx,
                                   boundaryFaceIdx);
                Valgrind::CheckDefined(dirichletVal);
            }

            // copy the dirichlet value for the current equation
            // to the global solution.
            //
            // TODO: we should propably use the sum weighted by the
            //       sub-control volume instead of just overwriting
            //       the previous values...
            (*u)[globalVertexIdx][eqIdx] = dirichletVal[boundaryTypes.eqToDirichletIndex(eqIdx)];
        }
    }

    Implementation &asImp_()
    { return *static_cast<Implementation*>(this); }
    const Implementation &asImp_() const
    { return *static_cast<const Implementation*>(this); }

    // the problem we want to solve. defines the constitutive
    // relations, material laws, etc.
    Problem     &problem_;

    // the grid view for which we need a solution
    const GridView gridView_;

    // mapper for the entities of a solution to their indices
    const DofEntityMapper dofEntityMapper_;

    // mapper for the vertices to indices
    const VertexMapper vertexMapper_;

    // mapper for the elements to indices
    const ElementMapper elementMapper_;

    // the solution we are looking for

    // calculates the local jacobian matrix for a given element
    LocalJacobian     localJacobian_;
    // Linearizes the problem at the current time step using the
    // local jacobian
    JacobianAssembler *jacAsm_;

    // cur is the current solution, prev the solution of the previous
    // time step
    SolutionFunction *uCur_;
    SolutionFunction *uPrev_;
    // the right hand side
    SolutionFunction  *f_;

    bool wasRestarted_;
};
}

#endif

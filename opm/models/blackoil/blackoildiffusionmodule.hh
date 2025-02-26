// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.

  Consult the COPYING file in the top-level source directory of this
  module for the precise wording of the license and the list of
  copyright holders.
*/
/*!
 * \file
 *
 * \brief Classes required for molecular diffusion.
 */
#ifndef EWOMS_DIFFUSION_MODULE_HH
#define EWOMS_DIFFUSION_MODULE_HH

#include <opm/models/discretization/common/fvbaseproperties.hh>

#include <opm/material/common/Valgrind.hpp>

#include <dune/common/fvector.hh>

#include <stdexcept>

namespace Opm {

/*!
 * \ingroup Diffusion
 * \class Opm::BlackOilDiffusionModule
 * \brief Provides the auxiliary methods required for consideration of the
 * diffusion equation.
 */
template <class TypeTag, bool enableDiffusion>
class BlackOilDiffusionModule;

template <class TypeTag, bool enableDiffusion>
class BlackOilDiffusionExtensiveQuantities;

/*!
 * \copydoc Opm::BlackOilDiffusionModule
 */
template <class TypeTag>
class BlackOilDiffusionModule<TypeTag, /*enableDiffusion=*/false>
{
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;
    using RateVector = GetPropType<TypeTag, Properties::RateVector>;

public:
    /*!
     * \brief Register all run-time parameters for the diffusion module.
     */
    static void registerParameters()
    {}

    /*!
     * \brief Adds the diffusive mass flux flux to the flux vector over a flux
     *        integration point.
      */
    template <class Context>
    static void addDiffusiveFlux(RateVector&,
                                 const Context&,
                                 unsigned,
                                 unsigned)
    {}
};

/*!
 * \copydoc Opm::BlackOilDiffusionModule
 */
template <class TypeTag>
class BlackOilDiffusionModule<TypeTag, /*enableDiffusion=*/true>
{
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using Evaluation = GetPropType<TypeTag, Properties::Evaluation>;
    using RateVector = GetPropType<TypeTag, Properties::RateVector>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;
    using Indices = GetPropType<TypeTag, Properties::Indices>;

    enum { numPhases = FluidSystem::numPhases };
    enum { numComponents = FluidSystem::numComponents };
    enum { conti0EqIdx = Indices::conti0EqIdx };

    using Toolbox = MathToolbox<Evaluation>;

public:
    using ExtensiveQuantities = BlackOilDiffusionExtensiveQuantities<TypeTag,true>;
    /*!
     * \brief Register all run-time parameters for the diffusion module.
     */
    static void registerParameters()
    {}

    /*!
     * \brief Adds the mass flux due to molecular diffusion to the flux vector over the
     *        flux integration point.
     */
    template <class Context>
    static void addDiffusiveFlux(RateVector& flux, const Context& context,
                                 unsigned spaceIdx, unsigned timeIdx)
    {
        // Only work if diffusion is enabled run-time by DIFFUSE in the deck
        if(!FluidSystem::enableDiffusion())
            return;
        const auto& extQuants = context.extensiveQuantities(spaceIdx, timeIdx);
        const auto& fluidStateI = context.intensiveQuantities(extQuants.interiorIndex(), timeIdx).fluidState();
        const auto& fluidStateJ = context.intensiveQuantities(extQuants.exteriorIndex(), timeIdx).fluidState();
        const auto& diffusivity = extQuants.diffusivity();
        const auto& effectiveDiffusionCoefficient = extQuants.effectiveDiffusionCoefficient();
        addDiffusiveFlux(flux, fluidStateI, fluidStateJ, diffusivity, effectiveDiffusionCoefficient);
    }

    template<class FluidState,class EvaluationArray>
    static void addDiffusiveFlux(RateVector& flux,
                                 const FluidState& fluidStateI,
                                 const FluidState& fluidStateJ,
                                 const Evaluation& diffusivity,
                                 const EvaluationArray& effectiveDiffusionCoefficient)
    {
        unsigned pvtRegionIndex = fluidStateI.pvtRegionIndex();
        for (unsigned phaseIdx = 0; phaseIdx < numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }

            // no diffusion in water for blackoil models
            if (!FluidSystem::enableDissolvedGasInWater() && FluidSystem::waterPhaseIdx == phaseIdx) {
                continue;
            }

            // no diffusion in gas phase in water + gas system.
            if (FluidSystem::gasPhaseIdx == phaseIdx && !FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                continue;
            }

            // arithmetic mean of the phase's b factor weighed by saturation
            Evaluation bSAvg = fluidStateI.saturation(phaseIdx) * fluidStateI.invB(phaseIdx);
            bSAvg += Toolbox::value(fluidStateJ.saturation(phaseIdx)) * Toolbox::value(fluidStateJ.invB(phaseIdx));
            bSAvg /= 2;

            // phase not present, skip
            if(bSAvg < 1.0e-6)
                continue;
            Evaluation convFactor = 1.0;
            Evaluation diffR = 0.0;
            if (FluidSystem::enableDissolvedGas() && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) && phaseIdx == FluidSystem::oilPhaseIdx) {
                Evaluation rsAvg = (fluidStateI.Rs() + Toolbox::value(fluidStateJ.Rs())) / 2;
                convFactor = 1.0 / (toMolFractionGasOil(pvtRegionIndex) + rsAvg);
                diffR = fluidStateI.Rs() - Toolbox::value(fluidStateJ.Rs());
            }
            if (FluidSystem::enableVaporizedOil() && FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && phaseIdx == FluidSystem::gasPhaseIdx) {
                Evaluation rvAvg = (fluidStateI.Rv() + Toolbox::value(fluidStateJ.Rv())) / 2;
                convFactor = toMolFractionGasOil(pvtRegionIndex) / (1.0 + rvAvg*toMolFractionGasOil(pvtRegionIndex));
                diffR = fluidStateI.Rv() - Toolbox::value(fluidStateJ.Rv());
            }
            if (FluidSystem::enableDissolvedGasInWater() && phaseIdx == FluidSystem::waterPhaseIdx) {
                Evaluation rsAvg = (fluidStateI.Rsw() + Toolbox::value(fluidStateJ.Rsw())) / 2;
                convFactor = 1.0 / (toMolFractionGasWater(pvtRegionIndex) + rsAvg);
                diffR = fluidStateI.Rsw() - Toolbox::value(fluidStateJ.Rsw());
            }

            // mass flux of solvent component (oil in oil or gas in gas)
            unsigned solventCompIdx = FluidSystem::solventComponentIndex(phaseIdx);
            unsigned activeSolventCompIdx = Indices::canonicalToActiveComponentIndex(solventCompIdx);
            flux[conti0EqIdx + activeSolventCompIdx] +=
                    - bSAvg
                    * convFactor
                    * diffR
                    * diffusivity
                    * effectiveDiffusionCoefficient[phaseIdx][solventCompIdx];

            // mass flux of solute component (gas in oil or oil in gas)
            unsigned soluteCompIdx = FluidSystem::soluteComponentIndex(phaseIdx);
            unsigned activeSoluteCompIdx = Indices::canonicalToActiveComponentIndex(soluteCompIdx);
            flux[conti0EqIdx + activeSoluteCompIdx] +=
                    bSAvg
                    * diffR
                    * convFactor
                    * diffusivity
                    * effectiveDiffusionCoefficient[phaseIdx][soluteCompIdx];
        }
    }

private:
    static Scalar toMolFractionGasOil (unsigned regionIdx) {
        Scalar mMOil = FluidSystem::molarMass(FluidSystem::oilCompIdx, regionIdx);
        Scalar rhoO = FluidSystem::referenceDensity(FluidSystem::oilPhaseIdx, regionIdx);
        Scalar mMGas = FluidSystem::molarMass(FluidSystem::gasCompIdx, regionIdx);
        Scalar rhoG = FluidSystem::referenceDensity(FluidSystem::gasPhaseIdx, regionIdx);
        return rhoO * mMGas / (rhoG * mMOil);
    }
    static Scalar toMolFractionGasWater (unsigned regionIdx) {
        Scalar mMWater = FluidSystem::molarMass(FluidSystem::waterCompIdx, regionIdx);
        Scalar rhoW = FluidSystem::referenceDensity(FluidSystem::waterPhaseIdx, regionIdx);
        Scalar mMGas = FluidSystem::molarMass(FluidSystem::gasCompIdx, regionIdx);
        Scalar rhoG = FluidSystem::referenceDensity(FluidSystem::gasPhaseIdx, regionIdx);
        return rhoW * mMGas / (rhoG * mMWater);
    }
};

/*!
 * \ingroup Diffusion
 * \class Opm::BlackOilDiffusionIntensiveQuantities
 *
 * \brief Provides the volumetric quantities required for the
 *        calculation of molecular diffusive fluxes.
 */
template <class TypeTag, bool enableDiffusion>
class BlackOilDiffusionIntensiveQuantities;

/*!
 * \copydoc Opm::DiffusionIntensiveQuantities
 */
template <class TypeTag>
class BlackOilDiffusionIntensiveQuantities<TypeTag, /*enableDiffusion=*/false>
{
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using ElementContext = GetPropType<TypeTag, Properties::ElementContext>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;

public:
    /*!
     * \brief Returns the tortuousity of the sub-domain of a fluid
     *        phase in the porous medium.
     */
    Scalar tortuosity(unsigned) const
    {
        throw std::logic_error("Method tortuosity() does not make sense "
                               "if diffusion is disabled");
    }

    /*!
     * \brief Returns the molecular diffusion coefficient for a
     *        component in a phase.
     */
    Scalar diffusionCoefficient(unsigned, unsigned) const
    {
        throw std::logic_error("Method diffusionCoefficient() does not "
                               "make sense if diffusion is disabled");
    }

    /*!
     * \brief Returns the effective molecular diffusion coefficient of
     *        the porous medium for a component in a phase.
     */
    Scalar effectiveDiffusionCoefficient(unsigned, unsigned) const
    {
        throw std::logic_error("Method effectiveDiffusionCoefficient() "
                               "does not make sense if diffusion is disabled");
    }

protected:
    /*!
     * \brief Update the quantities required to calculate diffusive
     *        mass fluxes.
     */
    template <class FluidState>
    void update_(FluidState&,
                 typename FluidSystem::template ParameterCache<typename FluidState::Scalar>&,
                 const ElementContext&,
                 unsigned,
                 unsigned)
    { }
};

/*!
 * \copydoc Opm::DiffusionIntensiveQuantities
 */
template <class TypeTag>
class BlackOilDiffusionIntensiveQuantities<TypeTag, /*enableDiffusion=*/true>
{
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using Evaluation = GetPropType<TypeTag, Properties::Evaluation>;
    using ElementContext = GetPropType<TypeTag, Properties::ElementContext>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;
    using IntensiveQuantities = GetPropType<TypeTag, Properties::IntensiveQuantities>;
    enum { numPhases = FluidSystem::numPhases };
    enum { numComponents = FluidSystem::numComponents };

public:
    BlackOilDiffusionIntensiveQuantities() = default;
    BlackOilDiffusionIntensiveQuantities(BlackOilDiffusionIntensiveQuantities&&) noexcept = default;
    BlackOilDiffusionIntensiveQuantities(const BlackOilDiffusionIntensiveQuantities&) = default;

    BlackOilDiffusionIntensiveQuantities& operator=(BlackOilDiffusionIntensiveQuantities&&) noexcept = default;

    BlackOilDiffusionIntensiveQuantities&
    operator=(const BlackOilDiffusionIntensiveQuantities& rhs)
    {
        if (this == &rhs) return *this;

        if (FluidSystem::enableDiffusion()) {
          std::copy(rhs.tortuosity_, rhs.tortuosity_ + numPhases, tortuosity_);
          for (size_t i = 0; i < numPhases; ++i) {
              std::copy(rhs.diffusionCoefficient_[i],
                        rhs.diffusionCoefficient_[i]+numComponents,
                        diffusionCoefficient_[i]);
          }
      }
      return *this;
    }
    /*!
     * \brief Returns the molecular diffusion coefficient for a
     *        component in a phase.
     */
    Evaluation diffusionCoefficient(unsigned phaseIdx, unsigned compIdx) const
    { return diffusionCoefficient_[phaseIdx][compIdx]; }

    /*!
     * \brief Returns the tortuousity of the sub-domain of a fluid
     *        phase in the porous medium.
     */
    Evaluation tortuosity(unsigned phaseIdx) const
    { return tortuosity_[phaseIdx]; }

    /*!
     * \brief Returns the effective molecular diffusion coefficient of
     *        the porous medium for a component in a phase.
     */
    Evaluation effectiveDiffusionCoefficient(unsigned phaseIdx, unsigned compIdx) const
    {
        // For the blackoil model tortuosity is disabled.
        // TODO add a run-time parameter to enable tortuosity
        static bool enableTortuosity = false;
        if (enableTortuosity)
            return tortuosity_[phaseIdx] * diffusionCoefficient_[phaseIdx][compIdx];

        return diffusionCoefficient_[phaseIdx][compIdx];
    }

protected:
    /*!
     * \brief Update the quantities required to calculate diffusive
     *        mass fluxes.
     */
    template <class FluidState>
    void update_(FluidState& fluidState,
                 typename FluidSystem::template ParameterCache<typename FluidState::Scalar>& paramCache,
                 const ElementContext& elemCtx,
                 unsigned dofIdx,
                 unsigned timeIdx)
    {
        // Only work if diffusion is enabled run-time by DIFFUSE in the deck
        if(!FluidSystem::enableDiffusion())
            return;
        const auto& intQuants = elemCtx.intensiveQuantities(dofIdx, timeIdx);
        update_(fluidState, paramCache, intQuants);
    }

    template<class FluidState>
    void update_(FluidState& fluidState,
                 typename FluidSystem::template ParameterCache<typename FluidState::Scalar>& paramCache,
                 const IntensiveQuantities& intQuants) {
        using Toolbox = MathToolbox<Evaluation>;

        for (unsigned phaseIdx = 0; phaseIdx < numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }

            // no diffusion in water for blackoil models
            if (!FluidSystem::enableDissolvedGasInWater() && FluidSystem::waterPhaseIdx == phaseIdx) {
                continue;
            }

            // Based on Millington, R. J., & Quirk, J. P. (1961).
            // \Note: it is possible to use NumericalConstants later
            //  constexpr auto& numconst = GetPropValue<TypeTag, Properties::NumericalConstants>;
            constexpr double myeps = 0.0001; //numconst.blackoildiffusionmoduleeps;
            const Evaluation& base =
                Toolbox::max(myeps, //0.0001,
                             intQuants.porosity()
                             * intQuants.fluidState().saturation(phaseIdx));
            tortuosity_[phaseIdx] =
                1.0 / (intQuants.porosity() * intQuants.porosity())
                * Toolbox::pow(base, 10.0/3.0);

            for (unsigned compIdx = 0; compIdx < numComponents; ++compIdx) {
                diffusionCoefficient_[phaseIdx][compIdx] =
                    FluidSystem::diffusionCoefficient(fluidState,
                                                      paramCache,
                                                      phaseIdx,
                                                      compIdx);
            }
        }
    }

private:
    Evaluation tortuosity_[numPhases];
    Evaluation diffusionCoefficient_[numPhases][numComponents];
};

/*!
 * \ingroup Diffusion
 * \class Opm::BlackOilDiffusionExtensiveQuantities
 *
 * \brief Provides the quantities required to calculate diffusive mass fluxes.
 */
template <class TypeTag, bool enableDiffusion>
class BlackOilDiffusionExtensiveQuantities;

/*!
 * \copydoc Opm::DiffusionExtensiveQuantities
 */
template <class TypeTag>
class BlackOilDiffusionExtensiveQuantities<TypeTag, /*enableDiffusion=*/false>
{
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using Evaluation = GetPropType<TypeTag, Properties::Evaluation>;
    using ElementContext = GetPropType<TypeTag, Properties::ElementContext>;

protected:
    /*!
     * \brief Update the quantities required to calculate
     *        the diffusive mass fluxes.
     */
    void update_(const ElementContext&,
                 unsigned,
                 unsigned)
    {}

    template <class Context, class FluidState>
    void updateBoundary_(const Context&,
                         unsigned,
                         unsigned,
                         const FluidState&)
    {}

public:
    /*!
     * \brief The diffusivity the face.
     *
     */
    const Scalar& diffusivity() const
    {
        throw std::logic_error("The method diffusivity() does not "
                               "make sense if diffusion is disabled.");
    }

    /*!
     * \brief The effective diffusion coeffcient of a component in a
     *        fluid phase at the face's integration point
     *
     * \copydoc Doxygen::phaseIdxParam
     * \copydoc Doxygen::compIdxParam
     */
    const Evaluation& effectiveDiffusionCoefficient(unsigned,
                                                    unsigned) const
    {
        throw std::logic_error("The method effectiveDiffusionCoefficient() "
                               "does not make sense if diffusion is disabled.");
    }

};

/*!
 * \copydoc Opm::BlackOilDiffusionExtensiveQuantities
 */
template <class TypeTag>
class BlackOilDiffusionExtensiveQuantities<TypeTag, /*enableDiffusion=*/true>
{
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using Evaluation = GetPropType<TypeTag, Properties::Evaluation>;
    using ElementContext = GetPropType<TypeTag, Properties::ElementContext>;
    using GridView = GetPropType<TypeTag, Properties::GridView>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;
    using Toolbox = MathToolbox<Evaluation>;
    using IntensiveQuantities = GetPropType<TypeTag, Properties::IntensiveQuantities>;

    enum { dimWorld = GridView::dimensionworld };
    enum { numPhases = getPropValue<TypeTag, Properties::NumPhases>() };
    enum { numComponents = getPropValue<TypeTag, Properties::NumComponents>() };

    using DimVector = Dune::FieldVector<Scalar, dimWorld>;
    using DimEvalVector = Dune::FieldVector<Evaluation, dimWorld>;
public:
    using EvaluationArray = Evaluation[numPhases][numComponents];
protected:
    /*!
     * \brief Update the quantities required to calculate
     *        the diffusive mass fluxes.
     */
    void update_(const ElementContext& elemCtx, unsigned faceIdx, unsigned timeIdx)
    {
        // Only work if diffusion is enabled run-time by DIFFUSE in the deck
        if(!FluidSystem::enableDiffusion())
            return;

        const auto& stencil = elemCtx.stencil(timeIdx);
        const auto& face = stencil.interiorFace(faceIdx);
        const auto& extQuants = elemCtx.extensiveQuantities(faceIdx, timeIdx);
        const auto& intQuantsInside = elemCtx.intensiveQuantities(extQuants.interiorIndex(), timeIdx);
        const auto& intQuantsOutside = elemCtx.intensiveQuantities(extQuants.exteriorIndex(), timeIdx);

        const Scalar diffusivity = elemCtx.problem().diffusivity(elemCtx, face.interiorIndex(), face.exteriorIndex());
        const Scalar faceArea = face.area();
        diffusivity_ = diffusivity / faceArea;
        update(effectiveDiffusionCoefficient_, intQuantsInside, intQuantsOutside);
        Valgrind::CheckDefined(diffusivity_);
    }

public:
    static void update(EvaluationArray& effectiveDiffusionCoefficient,
                       const IntensiveQuantities& intQuantsInside,
                       const IntensiveQuantities& intQuantsOutside) {
        // opm-models expects per area flux
        for (unsigned phaseIdx = 0; phaseIdx < numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }
            // no diffusion in water for blackoil models
            if (!FluidSystem::enableDissolvedGasInWater() && FluidSystem::waterPhaseIdx == phaseIdx) {
                continue;
            }
            for (unsigned compIdx = 0; compIdx < numComponents; ++compIdx) {
                // use the arithmetic average for the effective
                // diffusion coefficients.
                effectiveDiffusionCoefficient[phaseIdx][compIdx] = 0.5 *
                    ( intQuantsInside.effectiveDiffusionCoefficient(phaseIdx, compIdx) +
                      intQuantsOutside.effectiveDiffusionCoefficient(phaseIdx, compIdx) );
                Valgrind::CheckDefined(effectiveDiffusionCoefficient[phaseIdx][compIdx]);
            }
        }
    }
protected:
    template <class Context, class FluidState>
    void updateBoundary_(const Context&,
                         unsigned,
                         unsigned,
                         const FluidState&)
    {
        throw std::runtime_error("Not implemented: Diffusion across boundary not implemented for blackoil");
    }

public:
    /*!
     * \brief The diffusivity of the face.
     *
     * \copydoc Doxygen::phaseIdxParam
     * \copydoc Doxygen::compIdxParam
     */
    const Scalar& diffusivity() const
    { return diffusivity_; }

    /*!
     * \brief The effective diffusion coeffcient of a component in a
     *        fluid phase at the face's integration point
     *
     * \copydoc Doxygen::phaseIdxParam
     * \copydoc Doxygen::compIdxParam
     */
    const Evaluation& effectiveDiffusionCoefficient(unsigned phaseIdx, unsigned compIdx) const
    { return effectiveDiffusionCoefficient_[phaseIdx][compIdx]; }

    const auto& effectiveDiffusionCoefficient() const{
        return effectiveDiffusionCoefficient_;
    }

private:
    Scalar diffusivity_;
    EvaluationArray effectiveDiffusionCoefficient_;
};

} // namespace Opm

#endif

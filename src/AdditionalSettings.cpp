#include "AdditionalSettings.h"

#include <CoreInterface.h>
#include <Dataset.h>
#include <DataType.h>
#include <LinkedData.h>
#include <Set.h>
#include <PointData/PointData.h>
#include <util/StyledIcon.h>

#include <QDialog>
#include <QGridLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <ranges>
#include <utility>
#include <vector>

// 
// Utility functions
//

std::pair<const mv::LinkedData*, unsigned int> getSelectionMapping(const mv::Dataset<Points>& source, const mv::Dataset<Points>& target, LinkedDataCondition checkMapping) {
    const std::vector<mv::LinkedData>& linkedDatas = source->getLinkedData();

    if (linkedDatas.empty())
        return { nullptr, 0 };

    // find linked data between source and target OR source and target's parent, if target is derived and they have the same number of points
    if (const auto result = std::ranges::find_if(
        linkedDatas,
        [&target, &checkMapping](const mv::LinkedData& linkedData) -> bool {
            return checkMapping(linkedData, target);
        });
        result != linkedDatas.end())
    {
        return { &(*result), target->getNumPoints() };
    }

    return { nullptr, 0 };
}

std::pair<const mv::LinkedData*, unsigned int> getSelectionMappingOtherToCurrent(const mv::Dataset<Points>& other, const mv::Dataset<Points>& current) {
    auto testTarget = [](const mv::LinkedData& linkedData, const mv::Dataset<Points>& current) -> bool {
        return linkedData.getTargetDataset() == current;
        };

    return getSelectionMapping(other, current, testTarget);
}

bool checkSurjectiveMapping(const mv::LinkedData* linkedData, const std::uint32_t numPointsInTarget) {
    if (linkedData == nullptr)
        return false;

    const std::map<std::uint32_t, std::vector<std::uint32_t>>& linkedMap = linkedData->getMapping().getMap();

    std::vector<bool> found(numPointsInTarget, false);
    std::uint32_t count = 0;

    for (const auto& [key, vec] : linkedMap) {
        for (std::uint32_t val : vec) {
            if (val >= numPointsInTarget) continue; // Skip values that are too large

            if (!found[val]) {
                found[val] = true;
                if (++count == numPointsInTarget)
                    return true;
            }
        }
    }

    return false; // The previous loop would have returned early if the entire taget set was covered
}

bool checkSelectionMapping(const mv::Dataset<Points>& other, const mv::Dataset<Points>& current) {

    // Check if there is a mapping
    auto [mapping, numTargetPoints] = getSelectionMappingOtherToCurrent(other, current);

    if (!mapping)
        return false;

    if (numTargetPoints != current->getNumPoints())
        return false;

    return true;
}

// 
// AdditionalSettingsDialog
//


AdditionalSettingsDialog::AdditionalSettingsDialog() :
    QDialog(),
    _okButton(this, "Ok"),
    _selectionMappingSourcePicker(this, "Selection mapping source")
{
    setWindowTitle("Additional DE Viewer settings");
    setWindowIcon(mv::util::StyledIcon("gears"));
    setModal(false);

    connect(&_okButton, &mv::gui::TriggerAction::triggered, this, &QDialog::accept);
}

void AdditionalSettingsDialog::populateAndOpenDialog() {
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    int row = 0;

    _selectionMappingSourcePicker.setFilterFunction([this](mv::Dataset<mv::DatasetImpl> dataset) -> bool {
        return checkSelectionMapping(dataset, _currentData);
        });

    //const mv::Datasets dataSets = mv::data().getAllDatasets(std::vector<mv::DataType> { PointType });
    //_selectionMappingSourcePicker.setDatasets(dataSets);

    layout->addWidget(_selectionMappingSourcePicker.createLabelWidget(this), ++row, 0, 1, 1);
    layout->addWidget(_selectionMappingSourcePicker.createWidget(this), row, 1, 1, -1);

    layout->addWidget(_okButton.createWidget(this), ++row, 0, 1, -1, Qt::AlignRight);

    setLayout(layout);

    show();
}

void AdditionalSettingsDialog::fromVariantMap(const QVariantMap& variantMap)
{
    _selectionMappingSourcePicker.fromParentVariantMap(variantMap);
}

QVariantMap AdditionalSettingsDialog::toVariantMap() const
{
    QVariantMap variantMap;

    _selectionMappingSourcePicker.insertIntoVariantMap(variantMap);

    return variantMap;
}


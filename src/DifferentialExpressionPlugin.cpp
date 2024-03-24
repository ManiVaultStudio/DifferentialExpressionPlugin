#include "DifferentialExpressionPlugin.h"

#include <event/Event.h>

#include <DatasetsMimeData.h>

#include <QDebug>
#include <QMimeData>
#include <QFile>

#include <QPushButton>

#include <iostream>

Q_PLUGIN_METADATA(IID "nl.BioVault.DifferentialExpressionPlugin")

using namespace mv;

DifferentialExpressionPlugin::DifferentialExpressionPlugin(const PluginFactory* factory) :
    ViewPlugin(factory),
    _loadedDatasetsAction(this, "Current dataset"),
    _dropWidget(nullptr),
    _points(),
    _currentDatasetName(),
    _currentDatasetNameLabel(new QLabel())
{
    // This line is mandatory if drag and drop behavior is required
    _currentDatasetNameLabel->setAcceptDrops(true);

    // Align text in the center
    _currentDatasetNameLabel->setAlignment(Qt::AlignCenter);
}

void DifferentialExpressionPlugin::init()
{
    _loadedDatasetsAction.initialize(this);

    // Create layout
    auto layout = new QVBoxLayout();

    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(_currentDatasetNameLabel);

    _tableWidget = new QTableWidget(10, 3, &this->getWidget());
    // Make 10 table items
    _geneTableItems.resize(10, nullptr);
    _diffTableItems.resize(10, nullptr);
    for (int i = 0; i < 10; i++)
    {
        _geneTableItems[i] = new QTableWidgetItem("");
        _diffTableItems[i] = new QTableWidgetItem("");
        _tableWidget->setItem(i, 0, _geneTableItems[i]);
        _tableWidget->setItem(i, 1, _diffTableItems[i]);
    }

    QStringList columnHeaders;
    columnHeaders.append("Gene");
    columnHeaders.append("Mean");
    columnHeaders.append("Std");
    _tableWidget->setHorizontalHeaderLabels(columnHeaders);

    layout->addWidget(_tableWidget);

    // Apply the layout
    getWidget().setLayout(layout);

    // Instantiate new drop widget
    _dropWidget = new DropWidget(_currentDatasetNameLabel);

    // Set the drop indicator widget (the widget that indicates that the view is eligible for data dropping)
    _dropWidget->setDropIndicatorWidget(new DropWidget::DropIndicatorWidget(&getWidget(), "No data loaded", "Drag an item from the data hierarchy and drop it here to visualize data..."));

    // Initialize the drop regions
    _dropWidget->initialize([this](const QMimeData* mimeData) -> DropWidget::DropRegions {
        // A drop widget can contain zero or more drop regions
        DropWidget::DropRegions dropRegions;

        const auto datasetsMimeData = dynamic_cast<const DatasetsMimeData*>(mimeData);

        if (datasetsMimeData == nullptr)
            return dropRegions;

        if (datasetsMimeData->getDatasets().count() > 1)
            return dropRegions;

        // Gather information to generate appropriate drop regions
        const auto dataset = datasetsMimeData->getDatasets().first();
        const auto datasetGuiName = dataset->getGuiName();
        const auto datasetId = dataset->getId();
        const auto dataType = dataset->getDataType();
        const auto dataTypes = DataTypes({ PointType });

        // Visually indicate if the dataset is of the wrong data type and thus cannot be dropped
        if (!dataTypes.contains(dataType)) {
            dropRegions << new DropWidget::DropRegion(this, "Incompatible data", "This type of data is not supported", "exclamation-circle", false);
        }
        else {

            // Get points dataset from the core
            auto candidateDataset = mv::data().getDataset<Points>(datasetId);

            // Accept points datasets drag and drop
            if (dataType == PointType) {
                const auto description = QString("Load %1 into example view").arg(datasetGuiName);
                qDebug() << "Meep";
                if (_points == candidateDataset) {
                    
                    // Dataset cannot be dropped because it is already loaded
                    dropRegions << new DropWidget::DropRegion(this, "Warning", "Data already loaded", "exclamation-circle", false);
                }
                else {

                    // Dataset can be dropped
                    dropRegions << new DropWidget::DropRegion(this, "Points", description, "map-marker-alt", true, [this, candidateDataset]() {
                        qDebug() << "Meepmeep";
                        _points = candidateDataset;
                    });
                }
            }
        }

        return dropRegions;
    });

    // Respond when the name of the dataset in the dataset reference changes
    connect(&_points, &Dataset<Points>::guiNameChanged, this, [this]() {

        auto newDatasetName = _points->getGuiName();

        // Update the current dataset name label
        _currentDatasetNameLabel->setText(QString("Current points dataset: %1").arg(newDatasetName));

        // Only show the drop indicator when nothing is loaded in the dataset reference
        _dropWidget->setShowDropIndicator(newDatasetName.isEmpty());
    });

    _setFirstSelectionButton = new QPushButton("0 cells");
    _setSecondSelectionButton = new QPushButton("0 cells");
    _computeDiffExprButton = new QPushButton("Compute diff expression");

    connect(_setFirstSelectionButton, &QPushButton::pressed, this, [this]()
        {
            if (!_points.isValid())
                return;

            auto selectionDataset = _points->getSelection();
            std::vector<uint32_t> selectionIndices = selectionDataset->getSelectionIndices();

            selectionA = selectionIndices;
            _setFirstSelectionButton->setText(QString("%1 cells").arg(selectionIndices.size()));
            qDebug() << "Saved selection A.";
        });

    connect(_setSecondSelectionButton, &QPushButton::pressed, this, [this]()
        {
            if (!_points.isValid())
                return;

            auto selectionDataset = _points->getSelection();
            std::vector<uint32_t> selectionIndices = selectionDataset->getSelectionIndices();

            selectionB = selectionIndices;
            _setSecondSelectionButton->setText(QString("%1 cells").arg(selectionIndices.size()));
            qDebug() << "Saved selection B.";
        });

    connect(_computeDiffExprButton, &QPushButton::pressed, this, [this]()
        {
            if (!_points.isValid())
                return;

            auto selectionDataset = _points->getSelection();
            std::vector<uint32_t> selectionIndices = selectionDataset->getSelectionIndices();

            // Compute differential expr
            qDebug() << "Computing differential expression.";

            int numDimensions = _points->getNumDimensions();
            std::vector<float> meanA(numDimensions, 0);
            std::vector<float> meanB(numDimensions, 0);

            // Compute mean A
            for (uint32_t i = 0; i < selectionA.size(); i++)
            {
                std::vector<float> row = _points->row(i);
                for (int d = 0; d < numDimensions; d++)
                {
                    meanA[d] += row[d];
                }
            }
            // Compute mean B
            for (uint32_t i = 0; i < selectionB.size(); i++)
            {
                std::vector<float> row = _points->row(i);
                for (int d = 0; d < numDimensions; d++)
                {
                    meanB[d] += row[d];
                }
            }
            // Normalize and divide means by number of rows
            for (int d = 0; d < numDimensions; d++)
            {
                meanA[d] = (meanA[d] - minValues[d]) * rescaleValues[d];
                meanB[d] = (meanB[d] - minValues[d]) * rescaleValues[d];

                meanA[d] /= selectionA.size();
                meanB[d] /= selectionB.size();

                if (d < 100)
                    std::cout << meanA[d] << ", " << meanB[d] << std::endl;
            }
            std::cout << "Num dimensions: " << numDimensions << std::endl;

            // Compute difference in means
            std::vector<float> differences(numDimensions, 0);
            for (int d = 0; d < numDimensions; d++)
            {
                differences[d] = fabs(meanB[d] - meanA[d]);
            }

            // Sort differences
            std::vector<int> sortIndices(numDimensions);
            std::iota(sortIndices.begin(), sortIndices.end(), 0);
            std::stable_sort(sortIndices.begin(), sortIndices.end(), [&differences](size_t i1, size_t i2) {return differences[i1] > differences[i2]; });

            for (int i = 0; i < 10; i++)
            {
                _geneTableItems[i]->setText(_geneList[sortIndices[i]]);
                qDebug() << sortIndices[i];
                _diffTableItems[i]->setText(QString::number(differences[sortIndices[i]]));
            }
        });

    layout->addWidget(_setFirstSelectionButton);
    layout->addWidget(_setSecondSelectionButton);
    layout->addWidget(_computeDiffExprButton);

    // Load points when the pointer to the position dataset changes
    connect(&_points, &Dataset<Points>::changed, this, &DifferentialExpressionPlugin::positionDatasetChanged);

    // Alternatively, classes which derive from hdsp::EventListener (all plugins do) can also respond to events
    _eventListener.addSupportedEventType(static_cast<std::uint32_t>(EventType::DatasetAdded));
    _eventListener.addSupportedEventType(static_cast<std::uint32_t>(EventType::DatasetDataChanged));
    _eventListener.addSupportedEventType(static_cast<std::uint32_t>(EventType::DatasetRemoved));
    _eventListener.addSupportedEventType(static_cast<std::uint32_t>(EventType::DatasetDataSelectionChanged));
    _eventListener.registerDataEventByType(PointType, std::bind(&DifferentialExpressionPlugin::onDataEvent, this, std::placeholders::_1));

    // Read gene list
    QFile inputFile(":gene_symbols.csv");
    if (inputFile.open(QIODevice::ReadOnly))
    {
        QTextStream in(&inputFile);

        while (!in.atEnd())
        {
            QString line = in.readLine();

            _geneList.append(line);
        }
        inputFile.close();
    }
    else
    {
        qWarning() << "Genelist file was not found at location.";
    }
    qDebug() << "Loaded " << _geneList.size() << " genes.";
}

void DifferentialExpressionPlugin::onDataEvent(mv::DatasetEvent* dataEvent)
{
    // Get smart pointer to dataset that changed
    const auto changedDataSet = dataEvent->getDataset();

    // Get GUI name of the dataset that changed
    const auto datasetGuiName = changedDataSet->getGuiName();

    // The data event has a type so that we know what type of data event occurred (e.g. data added, changed, removed, renamed, selection changes)
    switch (dataEvent->getType()) {

        // A points dataset was added
        case EventType::DatasetAdded:
        {
            // Cast the data event to a data added event
            const auto dataAddedEvent = static_cast<DatasetAddedEvent*>(dataEvent);

            // Get the GUI name of the added points dataset and print to the console
            qDebug() << datasetGuiName << "was added";

            break;
        }

        default:
            break;
    }
}

void DifferentialExpressionPlugin::positionDatasetChanged()
{
    // Do not show the drop indicator if there is a valid point positions dataset
    _dropWidget->setShowDropIndicator(!_points.isValid());

    // Compute normalization
    int numDimensions = _points->getNumDimensions();
    minValues.resize(numDimensions, std::numeric_limits<float>::max());
    rescaleValues.resize(numDimensions, -std::numeric_limits<float>::max());

    // Find min and max values per dimension
    for (int i = 0; i < _points->getNumPoints(); i++)
    {
        std::vector<float> row = _points->row(i);
        for (int d = 0; d < numDimensions; d++)
        {
            if (row[d] < minValues[d])
            {
                minValues[d] = row[d];
            }
            if (row[d] > rescaleValues[d])
            {
                rescaleValues[d] = row[d];
            }
        }
    }

    // Compute rescale values
    for (int d = 0; d < numDimensions; d++)
    {
        float diff = (rescaleValues[d] - minValues[d]);
        if (diff != 0)
            rescaleValues[d] = 1.0f / (rescaleValues[d] - minValues[d]);
        else
            rescaleValues[d] = 1.0f;
    }
    qDebug() << "Done computing";
    qDebug() << rescaleValues[0] << minValues[0];
    qDebug() << rescaleValues[1] << minValues[1];
}

/******************************************************************************
 * Serialization
 ******************************************************************************/

void DifferentialExpressionPlugin::fromVariantMap(const QVariantMap& variantMap)
{
    ViewPlugin::fromVariantMap(variantMap);

    _loadedDatasetsAction.fromParentVariantMap(variantMap);

    //variantMapMustContain(variantMap, "SettingsAction");

    //getUI().getSettingsAction().fromVariantMap(variantMap["SettingsAction"].toMap());

    // Load in dataset
    // ...

    positionDatasetChanged();
}

QVariantMap DifferentialExpressionPlugin::toVariantMap() const
{
    QVariantMap variantMap = ViewPlugin::toVariantMap();

    _loadedDatasetsAction.insertIntoVariantMap(variantMap);

    return variantMap;
}

ViewPlugin* DifferentialExpressionPluginFactory::produce()
{
    return new DifferentialExpressionPlugin(this);
}

mv::DataTypes DifferentialExpressionPluginFactory::supportedDataTypes() const
{
    DataTypes supportedTypes;

    // This example analysis plugin is compatible with points datasets
    supportedTypes.append(PointType);

    return supportedTypes;
}

mv::gui::PluginTriggerActions DifferentialExpressionPluginFactory::getPluginTriggerActions(const mv::Datasets& datasets) const
{
    PluginTriggerActions pluginTriggerActions;

    const auto getPluginInstance = [this]() -> DifferentialExpressionPlugin* {
        return dynamic_cast<DifferentialExpressionPlugin*>(plugins().requestViewPlugin(getKind()));
    };

    const auto numberOfDatasets = datasets.count();

    if (numberOfDatasets >= 1 && PluginFactory::areAllDatasetsOfTheSameType(datasets, PointType)) {
        auto pluginTriggerAction = new PluginTriggerAction(const_cast<DifferentialExpressionPluginFactory*>(this), this, "Example", "View example data", getIcon(), [this, getPluginInstance, datasets](PluginTriggerAction& pluginTriggerAction) -> void {
            for (auto dataset : datasets)
                getPluginInstance();
        });

        pluginTriggerActions << pluginTriggerAction;
    }

    return pluginTriggerActions;
}

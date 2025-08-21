#include "DifferentialExpressionPlugin.h"

#include <event/Event.h>

#include <DatasetsMimeData.h>

#include <QDebug>
#include <QMimeData>
#include <QFile>
#include <QPushButton>
#include <QFileDialog>

#include "WordWrapHeaderView.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <limits>
#include <algorithm>

Q_PLUGIN_METADATA(IID "nl.BioVault.DifferentialExpressionPlugin")

using namespace mv;

namespace local
{
    static bool is_valid_QByteArray(const QByteArray& state)
    {
        QByteArray data = state;
        QDataStream stream(&data, QIODevice::ReadOnly);
        const int dataStreamVersion = QDataStream::Qt_5_0;
        stream.setVersion(dataStreamVersion);
        int marker;
        int ver;
        stream >> marker;
        stream >> ver;
        bool result = (stream.status() == QDataStream::Ok && (ver == 0));
        return result;
    }

    template <typename T>
    float fround(T n, int d)
    {
        assert(!std::numeric_limits<T>::is_integer); // this function should not be called on integer types
        return static_cast<float>(floor(n * pow(10., d) + 0.5) / pow(10., d));
    }
    template<typename T>
    bool is_exact_type(const QVariant& variant)
    {
        auto variantType = variant.metaType();
        auto requestedType = QMetaType::fromType<T>();
        return (variantType == requestedType);
    }
    template<typename T>
    T get_strict_value(const QVariant& variant)
    {
        if (is_exact_type<T>(variant))
            return variant.value<T>();
        else
        {
#ifdef _DEBUG
            qDebug() << "ClusterDifferentialExpressionPlugin: Error: requested " << QMetaType::fromType<T>().name() << " but value is of type " << variant.metaType().name();
#endif
            return T();
        }
    }
    template <typename FunctionObject>
    void visitElements(Dataset<Points> points, FunctionObject functionObject, const QString& description)
    {
        auto& task = points->getTask();
        task.setName(description);
        task.setProgressDescription(description);
        task.setProgressMode(Task::ProgressMode::Subtasks);
        task.setRunning();

        /* if (points->getSparseData().getValues().size())
         {
             const auto& _rowPointers = points->getSparseData().getIndexPointers();
             const auto& _colIndices = points->getSparseData().getColIndices();
             const auto& _values = points->getSparseData().getValues();

             const auto _numCols = points->getSparseData().getNumCols();
             const auto _numRows = points->getSparseData().getNumRows();

             task.setSubtasks(_numRows);
             for (std::size_t r = 0; r < _numRows; ++r)
             {
                 const size_t nzEnd = _rowPointers[r + 1];

                 for (std::ptrdiff_t nzIndex = _rowPointers[r]; nzIndex < nzEnd; nzIndex++)
                 {
                     const auto value = _values[nzIndex];
                     const auto column = _colIndices[nzIndex];
                     functionObject(r, column, value);
                 }

                 task.subtaskFinished(r);
             }

             task.setFinished();
         }
         else
         {*/
        const auto numDimensions = points->getNumDimensions();
        const auto numRows = points->getNumPoints();
        points->visitData([numRows, &task, numDimensions, functionObject](auto data)
            {
                task.setSubtasks(numRows);
                for (std::size_t r = 0; r < numRows; ++r)
                {
                    for (std::size_t column = 0; column < numDimensions; ++column)
                    {
                        const auto value = data[r][column];
                        functionObject(r, column, value);
                    }
                    task.subtaskFinished(r);
                }
                task.setFinished();
            });
        //}
    }
    template <typename RowRange, typename FunctionObject>
    void visitElements(Dataset<Points> points, const RowRange& rows, FunctionObject functionObject, const QString& description)
    {
        auto& task = points->getTask();
        task.setName(description);
        task.setProgressDescription(description);
        task.setProgressMode(Task::ProgressMode::Subtasks);
        task.setRunning();

        /*if(points->getSparseData().getValues().size())
        {
            const auto& _rowPointers = points->getSparseData().getIndexPointers();
            const auto& _colIndices = points->getSparseData().getColIndices();
            const auto& _values = points->getSparseData().getValues();

            auto _numCols = points->getSparseData().getNumCols();

            task.setSubtasks(rows.size());
            for (const auto r : rows)
            {
                const size_t nzEnd = _rowPointers[r + 1];
                for (std::ptrdiff_t nzIndex = _rowPointers[r]; nzIndex < nzEnd; nzIndex++)
                {
                    const auto value = _values[nzIndex];
                    const auto column = _colIndices[nzIndex];
                    functionObject(r, column, value);
                }

                task.subtaskFinished(r);
            }
            task.setFinished();
        }
        else
        {*/
        const auto numDimensions = points->getNumDimensions();
        points->visitData([&rows, &task, numDimensions, functionObject](auto data)
            {
                task.setSubtasks(rows.size());
                for (const auto r : rows)
                {
                    for (std::size_t column = 0; column < numDimensions; ++column)
                    {
                        const auto value = data[r][column];
                        functionObject(r, column, value);
                    }
                    task.subtaskFinished(r);
                }
                task.setFinished();
            });

        //}           
    }

}

DifferentialExpressionPlugin::DifferentialExpressionPlugin(const PluginFactory* factory) :
    ViewPlugin(factory),
    _loadedDatasetsAction(this, "Current dataset"),
    _dropWidget(nullptr),
    _points(),
    _currentDatasetName(),
    _currentDatasetNameLabel(new QLabel()),
    _filterOnIdAction(this, "Filter on Id"),
    _selectedIdAction(this, "Last selected Id"),
    _updateStatisticsAction(this, "Calculate Differential Expression"),
    _selectionTriggerActions(this, getGuiName(), "Set Selection %1"),
    _sortFilterProxyModel(new TableSortFilterProxyModel),
    _tableItemModel(new TableModel(nullptr, false)),
    _tableView(nullptr),
    _buttonProgressBar(nullptr),
    _copyToClipboardAction(&getWidget(), "Copy"),
    _saveToCsvAction(&getWidget(), "Save As..."),
    _normAction(&getWidget(), "Min-max normalization")
{
    // This line is mandatory if drag and drop behavior is required
    _currentDatasetNameLabel->setAcceptDrops(true);

    // Align text in the center
    _currentDatasetNameLabel->setAlignment(Qt::AlignCenter);

    _normAction.setToolTip("Rescale the data: (x - min) / (max - min)");

    { // copy to Clipboard

        //addTitleBarMenuAction(&_saveToCsvAction);
        _saveToCsvAction.setIcon(mv::util::StyledIcon("file-csv"));
        _saveToCsvAction.setShortcut(tr("Ctrl+S"));
        _saveToCsvAction.setShortcutContext(Qt::WidgetWithChildrenShortcut);

        connect(&_saveToCsvAction, &TriggerAction::triggered, this, [this]() -> void {
            this->writeToCSV();
            });
    }

    { // copy to Clipboard

       // addTitleBarMenuAction(&_copyToClipboardAction);
        _copyToClipboardAction.setIcon(mv::util::StyledIcon("copy"));
        _copyToClipboardAction.setShortcut(tr("Ctrl+C"));
        _copyToClipboardAction.setShortcutContext(Qt::WidgetWithChildrenShortcut);

        connect(&_copyToClipboardAction, &TriggerAction::triggered, this, [this]() -> void {
            this->_tableItemModel->copyToClipboard();
            });
    }

    _sortFilterProxyModel->setSourceModel(_tableItemModel.get());
    _filterOnIdAction.setSearchMode(true);
    _filterOnIdAction.setClearable(true);
    _filterOnIdAction.setPlaceHolderString("Filter by ID");

    connect(&_updateStatisticsAction, &TriggerAction::triggered, [this](const bool& var)
        {
            _tableItemModel->invalidate();
        });

    connect(&_filterOnIdAction, &mv::gui::StringAction::stringChanged, _sortFilterProxyModel, &TableSortFilterProxyModel::nameFilterChanged);

    connect(&_updateStatisticsAction, &mv::gui::TriggerAction::triggered, this, &DifferentialExpressionPlugin::computeDE);

    connect(&_normAction, &mv::gui::ToggleAction::toggled, this, [this]()
        {
            if (_normAction.isChecked())
                _norm = true;
            else
                _norm = false;

            _updateStatisticsAction.trigger();
        });

    _serializedActions.append(&_loadedDatasetsAction);
    _serializedActions.append(&_selectedIdAction);
    _serializedActions.append(&_filterOnIdAction);
    _serializedActions.append(&_copyToClipboardAction);
    _serializedActions.append(&_saveToCsvAction);
    _serializedActions.append(&_updateStatisticsAction);
    _serializedActions.append(&_selectionTriggerActions);

}

void DifferentialExpressionPlugin::init()
{
    QWidget& mainWidget = getWidget();
    _loadedDatasetsAction.initialize(this);

    // Create layout
    auto layout = new QVBoxLayout();

    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(_currentDatasetNameLabel);

    { // toolbar
        QWidget* filterWidget = _filterOnIdAction.createWidget(&mainWidget);
        filterWidget->setContentsMargins(0, 3, 0, 3);

        QHBoxLayout* toolBarLayout = new QHBoxLayout;
        toolBarLayout->addWidget(filterWidget, 8);
        toolBarLayout->addWidget(_normAction.createWidget(&mainWidget), 2);

        layout->addLayout(toolBarLayout);
    }

    { // table view

        _tableView = new TableView(&mainWidget);
        _tableView->setModel(_sortFilterProxyModel);
        _tableView->setSortingEnabled(true);
        _tableView->setSelectionMode(QAbstractItemView::SingleSelection);
        _tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        _tableView->setContextMenuPolicy(Qt::ActionsContextMenu);

        connect(_tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &DifferentialExpressionPlugin::tableView_selectionChanged);

        WordWrapHeaderView* horizontalHeader = new WordWrapHeaderView(Qt::Horizontal, _tableView, true);

        horizontalHeader->setFirstSectionMovable(false);
        horizontalHeader->setSectionsMovable(true);
        horizontalHeader->setSectionsClickable(true);
        horizontalHeader->sectionResizeMode(QHeaderView::Stretch);
        horizontalHeader->setSectionResizeMode(QHeaderView::Stretch);
        horizontalHeader->setStretchLastSection(true);
        horizontalHeader->setSortIndicator(1, Qt::AscendingOrder);
        horizontalHeader->setDefaultAlignment(Qt::AlignBottom | Qt::AlignLeft | Qt::Alignment(Qt::TextWordWrap));
        _tableView->setHorizontalHeader(horizontalHeader);
        layout->addWidget(_tableView);

        _tableView->addAction(&_saveToCsvAction);
        _tableView->addAction(&_copyToClipboardAction);
    }

    {// Progress bar and update button

        _buttonProgressBar = new ButtonProgressBar(&mainWidget, _updateStatisticsAction.createWidget(&mainWidget));
        _buttonProgressBar->setContentsMargins(0, 3, 0, 3);
        _buttonProgressBar->setProgressBarText("No Data Available");
        _buttonProgressBar->setButtonText("Calculate Differential Expression", Qt::black);

        connect(_tableItemModel.get(), &TableModel::statusChanged, _buttonProgressBar, &ButtonProgressBar::showStatus);

        layout->addWidget(_buttonProgressBar);
    }

    _tableItemModel->setHorizontalHeader(0, QString("ID"));
    _tableItemModel->setHorizontalHeader(1, QString("DE"));
    _tableItemModel->setHorizontalHeader(2, QString("Mean (Sel. 1)"));
    _tableItemModel->setHorizontalHeader(3, QString("Mean (Sel. 2)"));
    _tableItemModel->setHorizontalHeader(4, QString("Median (Sel. 1)"));
    _tableItemModel->setHorizontalHeader(5, QString("Median (Sel. 2)"));
    _tableItemModel->endModelBuilding();

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
            qDebug() << "ClusterDifferentialExpressionPlugin: Incompatible data: This type of data is not supported";
        }
        else
        {
            // Accept points datasets drag and drop
            if (dataType == PointType) {

                auto candidateDataset = mv::data().getDataset<Points>(datasetId);

                const auto description = QString("Load %1 into example view").arg(datasetGuiName);

                if (_points == candidateDataset) {

                    // Dataset cannot be dropped because it is already loaded
                    dropRegions << new DropWidget::DropRegion(this, "Warning", "Data already loaded", "exclamation-circle", false);
                    qDebug() << "ClusterDifferentialExpressionPlugin: Warning: Data already loaded";
                }
                else {

                    // Dataset can be dropped
                    dropRegions << new DropWidget::DropRegion(this, "Points", description, "map-marker-alt", true, [this, candidateDataset]() {

                        setPositionDataset(candidateDataset);

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

    for (std::size_t i = 0; i < MultiTriggerAction::Size; ++i)
    {
        _selectedCellsLabel[i].setText(QString("(%1 items)").arg(0));
        _selectedCellsLabel[i].setAlignment(Qt::AlignHCenter);
    }

    connect(_selectionTriggerActions.getTriggerAction(0), &TriggerAction::triggered, [this]()
        {
            if (!_points.isValid())
                return;

            auto selectionDataset = _points->getSelection();
            std::vector<uint32_t> selectionIndices = selectionDataset->getSelectionIndices();

            selectionA = selectionIndices;

            qDebug() << "ClusterDifferentialExpressionPlugin: Saved selection A.";
            _selectedCellsLabel[0].setText(QString("(%1 cells)").arg(selectionA.size()));
            if (selectionA.size() != 0 && selectionB.size() != 0)
                _buttonProgressBar->showStatus(TableModel::Status::OutDated);
        });

    connect(_selectionTriggerActions.getTriggerAction(1), &TriggerAction::triggered, [this]()
        {
            if (!_points.isValid())
                return;
            auto selectionDataset = _points->getSelection();
            std::vector<uint32_t> selectionIndices = selectionDataset->getSelectionIndices();
            _selectedCellsLabel[1].setText(QString("(%1 cells)").arg(selectionB.size()));
            selectionB = selectionIndices;

            qDebug() << "ClusterDifferentialExpressionPlugin: Saved selection B.";
            if (selectionA.size() != 0 && selectionB.size() != 0)
                _buttonProgressBar->showStatus(TableModel::Status::OutDated);
        });

    QGridLayout* selectionLayout = new QGridLayout();
    for (std::size_t i = 0; i < MultiTriggerAction::Size; ++i)
    {
        selectionLayout->addWidget(_selectionTriggerActions.getTriggerAction(i)->createWidget(&mainWidget), 0, i);
        selectionLayout->addWidget(&_selectedCellsLabel[i], 1, i);
        layout->addLayout(selectionLayout);
    }

     // Load points when the pointer to the position dataset changes
    connect(&_points, &Dataset<Points>::changed, this, &DifferentialExpressionPlugin::positionDatasetChanged);

}


void DifferentialExpressionPlugin::setPositionDataset(const mv::Dataset<Points>& newPoints)
{
    if (!newPoints.isValid())
    {
        qDebug() << "DifferentialExpressionPlugin Warning: invalid dataset!";
        return;
    }

    _points = newPoints;

    auto newDatasetName = _points->getGuiName();

    // Update the current dataset name label
    _currentDatasetNameLabel->setText(QString("Current points dataset: %1").arg(newDatasetName));

    // Only show the drop indicator when nothing is loaded in the dataset reference
    _dropWidget->setShowDropIndicator(newDatasetName.isEmpty());

}

void DifferentialExpressionPlugin::positionDatasetChanged()
{
    // Do not show the drop indicator if there is a valid point positions dataset
    _dropWidget->setShowDropIndicator(!_points.isValid());

    // Compute normalization
    const auto numDimensions = _points->getNumDimensions();
    
    // check if min and max need to be recomputed or are stored
    // first check if there are dimension statistics stored in the properties and if they contain min and max values
    QVariantMap dimensionStatisticsMap = _points->getProperty("Dimension Statistics").toMap();
    bool recompute = dimensionStatisticsMap.empty();
    recompute |= (dimensionStatisticsMap.constFind("min") == dimensionStatisticsMap.constEnd());
    recompute |= (dimensionStatisticsMap.constFind("max") == dimensionStatisticsMap.constEnd());

    if (recompute)
    {
        qDebug() << "ClusterDifferentialExpressionPlugin: Computing dimension ranges";
        minValues.resize(numDimensions, std::numeric_limits<float>::max());
        rescaleValues.resize(numDimensions, std::numeric_limits<float>::lowest());

        auto numPoints = _points->getNumPoints();

        std::vector<std::size_t> count(numDimensions, 0);

        local::visitElements(_points, [this, &count](auto row, auto column, auto value)->void
            {
                if (value > rescaleValues[column])
                    rescaleValues[column] = value;
                if (value < minValues[column])
                    minValues[column] = value;
                count[column]++;
            }, QString("Determine value ranges"));


        // check for potential 0 values and add them to the min and max range if needed
#pragma omp parallel for schedule(dynamic,1)
        for (std::ptrdiff_t d = 0; d < numDimensions; d++)
        {
            if (count[d] < numPoints)
            {
                if (minValues[d] > 0)
                    minValues[d] = 0;
                if (rescaleValues[d] < 0)
                    minValues[d] = 0;
            }
        }

        // store min and max values in the properties
        dimensionStatisticsMap["min"] = QVariantList(minValues.cbegin(), minValues.cend());
        dimensionStatisticsMap["max"] = QVariantList(rescaleValues.cbegin(), rescaleValues.cend());
        _points->setProperty("Dimension Statistics", dimensionStatisticsMap);
    }
    else
    {
        const QVariantList minList = dimensionStatisticsMap["min"].toList();
        const QVariantList maxList = dimensionStatisticsMap["max"].toList();
        recompute |= (minList.size() != numDimensions);
        recompute |= (maxList.size() != numDimensions);
        if (!recompute)
        {
            qDebug() << "ClusterDifferentialExpressionPlugin: Loading dimension ranges";
            // load them from properties
            minValues.resize(numDimensions);
            rescaleValues.resize(numDimensions);
#pragma  omp parallel for
            for (std::ptrdiff_t i = 0; i < numDimensions; ++i)
            {
                minValues[i]        = minList[i].toFloat();
                rescaleValues[i]    = maxList[i].toFloat();
            }

        }
    }

    // Compute rescale values
#pragma omp parallel for schedule(dynamic,1)
    for (std::ptrdiff_t d = 0; d < numDimensions; d++)
    {
        float diff = (rescaleValues[d] - minValues[d]);
        if (diff != 0)
            rescaleValues[d] = 1.0f / (rescaleValues[d] - minValues[d]);
        else
            rescaleValues[d] = 1.0f;
    }

    qDebug() << "DifferentialExpressionPlugin: Loaded " << numDimensions << " dimensions.";
}

void DifferentialExpressionPlugin::writeToCSV() const
{
    if (_tableItemModel.isNull())
        return;

    // Let the user chose the save path
    QSettings settings(QLatin1String{ "ManiVault" }, QLatin1String{ "Plugins/" } + getKind());
    const QLatin1String directoryPathKey("directoryPath");
    const auto directoryPath = settings.value(directoryPathKey).toString() + "/";

    QString fileName = QFileDialog::getSaveFileName(
        nullptr, tr("Save data set"), directoryPath + "DifferentialExpression.csv", tr("CSV file (*.csv);;All Files (*)"));

    // Only continue when the dialog has not been not canceled and the file name is non-empty.
    if (fileName.isNull() || fileName.isEmpty())
    {
        qDebug() << "ClusterDifferentialExpressionPlugin: No data written to disk - File name empty";
        return;
    }
    else
    {
        // store the directory name
        settings.setValue(directoryPathKey, QFileInfo(fileName).absolutePath());
    }

    QString csvString = _tableItemModel->createCSVString(',');
    if (csvString.isEmpty())
        return;
    QFile file(fileName);
    if (!file.open(QFile::WriteOnly | QFile::Truncate))
        return;
    QTextStream output(&file);
    output << csvString;
    file.close();
}

void DifferentialExpressionPlugin::computeDE()
{
    if (!_points.isValid())
        return;

    _tableItemModel->invalidate();
    auto selectionDataset = _points->getSelection();
    std::vector<uint32_t> selectionIndices = selectionDataset->getSelectionIndices();

    // Compute differential expr
    qDebug() << "ClusterDifferentialExpressionPlugin: Computing differential expression.";

    std::ptrdiff_t numDimensions = _points->getNumDimensions();
    std::vector<float> meanA(numDimensions, 0);
    std::vector<float> meanB(numDimensions, 0);

    // for median, collect per dimension values TODO: maybe look for median dynamically, instead of store the vectors
    std::vector<std::vector<float>> valuesA(numDimensions);
    std::vector<std::vector<float>> valuesB(numDimensions);
    std::vector<float> medianA(numDimensions, 0);
    std::vector<float> medianB(numDimensions, 0);

    // first compute the sum of values per dimension for selectionA and selectionB
    local::visitElements(_points, selectionA, [&meanA, &valuesA](auto row, auto column, auto value)
        {
            meanA[column] += value;
            valuesA[column].push_back(value);// for median
        }, QString("Computing mean expression values for Selection 1"));

    local::visitElements(_points, selectionB, [&meanB, &valuesB](auto row, auto column, auto value)
        {
            meanB[column] += value;
            valuesB[column].push_back(value); // for median
        }, QString("Computing mean expression values for Selection 2"));


#pragma omp parallel for schedule(dynamic,1)
    for (std::ptrdiff_t d = 0; d < numDimensions; d++)
    {
        // first divide means by number of rows
        meanA[d] /= selectionA.size();
        meanB[d] /= selectionB.size();

        // then min max - optional by toggle action
        if (_norm)
        {
            meanA[d] = (meanA[d] - minValues[d]) * rescaleValues[d];
            meanB[d] = (meanB[d] - minValues[d]) * rescaleValues[d];
        }

        // compute median
        auto& vectorA = valuesA[d];
        auto& vectorB = valuesB[d];

        std::nth_element(vectorA.begin(), vectorA.begin() + vectorA.size() / 2, vectorA.end());
        medianA[d] = vectorA[vectorA.size() / 2];

        std::nth_element(vectorB.begin(), vectorB.begin() + vectorB.size() / 2, vectorB.end());
        medianB[d] = vectorB[vectorB.size() / 2];

        // then min max - optional by toggle action
        if (_norm)
        {
            medianA[d] = (medianA[d] - minValues[d]) * rescaleValues[d];
            medianB[d] = (medianB[d] - minValues[d]) * rescaleValues[d];
        }
    }

    const int totalColumnCount = 6; // with two median columns
    const auto& dimensionNames = _points->getDimensionNames();

    _tableItemModel->startModelBuilding(totalColumnCount, numDimensions);
#pragma omp  parallel for schedule(dynamic,1)
    for (std::ptrdiff_t dimension = 0; dimension < numDimensions; ++dimension)
    {
        std::vector<QVariant> dataVector(totalColumnCount);
        dataVector[0] = dimensionNames[dimension];

        dataVector[1] = local::fround(meanA[dimension] - meanB[dimension], 3);
        dataVector[2] = local::fround(meanA[dimension], 3);
        dataVector[3] = local::fround(meanB[dimension], 3);
        dataVector[4] = local::fround(medianA[dimension], 3);
        dataVector[5] = local::fround(medianB[dimension], 3);

        _tableItemModel->setRow(dimension, dataVector, Qt::Unchecked, true);
    }
    _tableItemModel->endModelBuilding();
}

void DifferentialExpressionPlugin::tableView_clicked(const QModelIndex& index)
{
    if (_tableItemModel->status() != TableModel::Status::UpToDate)
        return;
    try
    {
        QModelIndex firstColumn = index.sibling(index.row(), 0);

        QString selectedGeneName = firstColumn.data().toString();
        QModelIndex temp = _sortFilterProxyModel->mapToSource(firstColumn);
        auto row = temp.row();
        _selectedIdAction.setString(selectedGeneName);
    }
    catch (...)
    {
        // catch everything
    }
}

void DifferentialExpressionPlugin::tableView_selectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
    tableView_clicked(selected.indexes().first());
}

/******************************************************************************
 * Serialization
 ******************************************************************************/

void DifferentialExpressionPlugin::fromVariantMap(const QVariantMap& variantMap)
{
    ViewPlugin::fromVariantMap(variantMap);

    for (auto action : _serializedActions)
    {
        if (variantMap.contains(action->getSerializationName()))
            action->fromParentVariantMap(variantMap);

    }

    QVariantMap propertiesMap = local::get_strict_value<QVariantMap>(variantMap.value("#Properties"));
    if (!propertiesMap.isEmpty())
    {
        {
            auto found = propertiesMap.constFind("TableViewHeaderState");
            if (found != propertiesMap.constEnd())
            {

                QVariant value = found.value();
                QString stateAsQString = local::get_strict_value<QString>(value);
                // When reading a QByteArray back from jsondocument it's a QString. to Convert it back to a QByteArray we need to use .toUtf8().
                QByteArray state = QByteArray::fromBase64(stateAsQString.toUtf8());
                assert(local::is_valid_QByteArray(state));
                _headerState = state;
            }
        }
    }

    setPositionDataset(_points);
}

QVariantMap DifferentialExpressionPlugin::toVariantMap() const
{
    QVariantMap variantMap = ViewPlugin::toVariantMap();

    for (auto action : _serializedActions)
    {
        assert(action->getSerializationName() != "#Properties");
        action->insertIntoVariantMap(variantMap);
    }

    // properties map
    QVariantMap propertiesMap;

    QByteArray headerState = _tableView->horizontalHeader()->saveState();
    propertiesMap["TableViewHeaderState"] = QString::fromUtf8(headerState.toBase64()); // encode the state with toBase64() and put it in a Utf8 QString since it will do that anyway. Best to be explicit in case it changes in the future
    variantMap["#Properties"] = propertiesMap;


    return variantMap;
}

// =============================================================================
// Factory
// =============================================================================

DifferentialExpressionPluginFactory::DifferentialExpressionPluginFactory()
{
    setIconByName("table");
}

ViewPlugin* DifferentialExpressionPluginFactory::produce()
{
    return new DifferentialExpressionPlugin(this);
}

mv::DataTypes DifferentialExpressionPluginFactory::supportedDataTypes() const
{
    return { PointType } ;
}

mv::gui::PluginTriggerActions DifferentialExpressionPluginFactory::getPluginTriggerActions(const mv::Datasets& datasets) const
{
    PluginTriggerActions pluginTriggerActions;

    const auto getPluginInstance = [this]() -> DifferentialExpressionPlugin* {
        return dynamic_cast<DifferentialExpressionPlugin*>(plugins().requestViewPlugin(getKind()));
        };

    const auto numberOfDatasets = datasets.count();

    if (numberOfDatasets >= 1 && PluginFactory::areAllDatasetsOfTheSameType(datasets, PointType)) {
        auto pluginTriggerAction = new PluginTriggerAction(const_cast<DifferentialExpressionPluginFactory*>(this), this, "Differential expression", "Compute differential expressions between two selections", StyledIcon(), [this, getPluginInstance, datasets](PluginTriggerAction& pluginTriggerAction) -> void {
            for (const auto& dataset : datasets)
                getPluginInstance()->setPositionDataset( dataset );
            });

        pluginTriggerActions << pluginTriggerAction;
    }

    return pluginTriggerActions;
}

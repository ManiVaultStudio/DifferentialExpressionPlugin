#include "DifferentialExpressionPlugin.h"

#include <DatasetsMimeData.h>

#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>

#include "AdditionalSettings.h"
#include "WordWrapHeaderView.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

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
            qDebug() << "DifferentialExpressionPlugin: Error: requested " << QMetaType::fromType<T>().name() << " but value is of type " << variant.metaType().name();
#endif
            return T();
        }
    }
    template <typename FunctionObject>
    void visitAllElements(Dataset<Points>& points, FunctionObject functionObject)
    {
        const auto numDimensions = points->getNumDimensions();
        const auto numRows = points->getNumPoints();
        points->visitData([numRows, numDimensions, functionObject](auto data)
            {
                for (std::size_t r = 0; r < numRows; ++r)
                {
                    for (std::size_t column = 0; column < numDimensions; ++column)
                    {
                        const auto value = data[r][column];
                        functionObject(r, column, value);
                    }
                }
            });
    }
    template <typename RowRange, typename FunctionObject>
    void visitElements(Dataset<Points>& points, const RowRange& rows, FunctionObject functionObject)
    {
        const auto numDimensions = points->getNumDimensions();
        points->visitData([&rows, numDimensions, functionObject](auto data)
            {
                const size_t numRows = rows.size();
                for (std::size_t localRow = 0; localRow < numRows; ++localRow)
                {
                    const auto globalRow = rows[localRow];
                    for (std::size_t column = 0; column < numDimensions; ++column)
                    {
                        const auto value = data[globalRow][column];
                        functionObject(globalRow, localRow, column, value);
                    }
                }
            });
    }

}

DifferentialExpressionPlugin::DifferentialExpressionPlugin(const PluginFactory* factory) :
    ViewPlugin(factory),
    _loadedDatasetsAction(this, "Current dataset"),
    _dropWidget(nullptr),
    _points(),
    _currentDatasetNameLabel(new QLabel()),
    _filterOnIdAction(this, "Filter on Id"),
    _selectedIdAction(this, "Last selected Id"),
    _updateStatisticsAction(this, "Calculate Differential Expression"),
    _setSelectionTriggerActions(this, "Set selection triggers", "Set selection %1"),
    _highlightSelectionTriggerActions(this, "Highlight selection triggers", "Highlight selection %1"),
    _sortFilterProxyModel(new TableSortFilterProxyModel),
    _totalTableColumns(0),
    _tableItemModel(new TableModel(nullptr, false)),
    _tableView(nullptr),
    _buttonProgressBar(nullptr),
    _copyToClipboardAction(&getWidget(), "Copy"),
    _saveToCsvAction(&getWidget(), "Save As..."),
    _additionalCalculationsAction(&getWidget(), "Additional calculations"),
    _thresholdExpressedAction(&getWidget(), "Threshold %expressed", 0.0f, 1.0f, 0.0f, 1),
    _normAction(&getWidget(), "Min-max normalization"),
    _currentSelectedDimension(this, "Selected dimension"),
    _openAdditionalSettingsAction(&getWidget(), "Open additional settings"),
    _additionalSettingsDialog()
{
    // This line is mandatory if drag and drop behavior is required
    _currentDatasetNameLabel->setAcceptDrops(true);

    // Align text in the center
    _currentDatasetNameLabel->setAlignment(Qt::AlignCenter);

    _normAction.setToolTip("Rescale the data: (selection_mean - global_min) / (global_max - global_min)");

    _thresholdExpressedAction.setDefaultWidgetFlags(DecimalAction::SpinBox);

    { // save to CSV

        //addTitleBarMenuAction(&_saveToCsvAction);
        _saveToCsvAction.setIcon(mv::util::StyledIcon("file-csv"));
        _saveToCsvAction.setShortcut(tr("Ctrl+S"));
        _saveToCsvAction.setShortcutContext(Qt::WidgetWithChildrenShortcut);

        connect(&_saveToCsvAction, &TriggerAction::triggered, this, [this]() -> void {
            writeToCSV();
            });
    }

    { // copy to Clipboard

       // addTitleBarMenuAction(&_copyToClipboardAction);
        _copyToClipboardAction.setIcon(mv::util::StyledIcon("copy"));
        _copyToClipboardAction.setShortcut(tr("Ctrl+C"));
        _copyToClipboardAction.setShortcutContext(Qt::WidgetWithChildrenShortcut);

        connect(&_copyToClipboardAction, &TriggerAction::triggered, this, [this]() -> void {
            _tableItemModel->copyToClipboard();
            });
    }

    { // additional settings dialog

        _openAdditionalSettingsAction.setIcon(mv::util::StyledIcon("gears"));
        _additionalSettingsDialog.setCurrentData(_points);

        connect(&_openAdditionalSettingsAction, &TriggerAction::triggered, this, [this]() -> void {
            _additionalSettingsDialog.setCurrentData(_points);
            _additionalSettingsDialog.show();
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

    connect(&_thresholdExpressedAction, &DecimalAction::valueChanged, this, [this](float value)
        {
            _updateStatisticsAction.trigger();
        });

    _serializedActions.append(&_loadedDatasetsAction);
    _serializedActions.append(&_selectedIdAction);
    _serializedActions.append(&_filterOnIdAction);
    _serializedActions.append(&_copyToClipboardAction);
    _serializedActions.append(&_saveToCsvAction);
    _serializedActions.append(&_updateStatisticsAction);
    _serializedActions.append(&_setSelectionTriggerActions);
    _serializedActions.append(&_highlightSelectionTriggerActions);
    _serializedActions.append(&_currentSelectedDimension);
    _serializedActions.append(&_openAdditionalSettingsAction);
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
        toolBarLayout->addWidget(filterWidget, 6);

        QWidget* normWidget = _normAction.createWidget(&mainWidget);
        QWidget* thresholdWidget = _thresholdExpressedAction.createWidget(&mainWidget);

        toolBarLayout->addWidget(_additionalCalculationsAction.createWidget(&mainWidget), 2);
        toolBarLayout->addWidget(normWidget, 2);
        toolBarLayout->addWidget(thresholdWidget, 2);

        bool showExtra = _additionalCalculationsAction.isChecked();
        normWidget->setVisible(showExtra);
        thresholdWidget->setVisible(showExtra);

        // Connect the toggle action to dynamically update their visibility
        connect(&_additionalCalculationsAction, &mv::gui::ToggleAction::toggled, this, [normWidget, thresholdWidget, this]() {
            _useAdditionalCalculations = _additionalCalculationsAction.isChecked();

            normWidget->setVisible(_useAdditionalCalculations);
            thresholdWidget->setVisible(_useAdditionalCalculations);

            // force norm and threhold go back to default, if hidden
            if (!_useAdditionalCalculations)
            {
                _normAction.setChecked(false);
                _thresholdExpressedAction.setValue(0.0f);
            }

            _updateStatisticsAction.trigger();
            });

        layout->addLayout(toolBarLayout);
    }

    { // table view

        _tableView = new TableView(&mainWidget);
        _tableView->setModel(_sortFilterProxyModel);
        _tableView->setSortingEnabled(true);
        _tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
        _tableView->addAction(&_openAdditionalSettingsAction);
    }

    {// Progress bar and update button

        _buttonProgressBar = new ButtonProgressBar(&mainWidget, _updateStatisticsAction.createWidget(&mainWidget));
        _buttonProgressBar->setContentsMargins(0, 3, 0, 3);
        _buttonProgressBar->setProgressBarText("No Data Available");
        _buttonProgressBar->setButtonText("Calculate Differential Expression", Qt::black);

        connect(_tableItemModel.get(), &TableModel::statusChanged, _buttonProgressBar, &ButtonProgressBar::showStatus);

        layout->addWidget(_buttonProgressBar);
    }

    _totalTableColumns = _additionalCalculationsAction.isChecked() ? 10 : 6;

    _tableItemModel->startModelBuilding(_totalTableColumns, 0);
    _tableItemModel->setHorizontalHeader(0, QString("ID"));
    _tableItemModel->setHorizontalHeader(1, QString("DE"));
    _tableItemModel->setHorizontalHeader(2, QString("Mean (Sel. 1)"));
    _tableItemModel->setHorizontalHeader(3, QString("Mean (Sel. 2)"));
    _tableItemModel->setHorizontalHeader(4, QString("Median (Sel. 1)"));
    _tableItemModel->setHorizontalHeader(5, QString("Median (Sel. 2)"));

    if (_totalTableColumns == 10) {
        _tableItemModel->setHorizontalHeader(6, QString("SD (Sel. 1)"));
        _tableItemModel->setHorizontalHeader(7, QString("SD (Sel. 2)"));
        _tableItemModel->setHorizontalHeader(8, QString("% Expressed (Sel. 1)"));
        _tableItemModel->setHorizontalHeader(9, QString("% Expressed (Sel. 2)"));
    }
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
            qDebug() << "DifferentialExpressionPlugin: Incompatible data: This type of data is not supported";
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
                    qDebug() << "DifferentialExpressionPlugin: Warning: Data already loaded";
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

    for (std::size_t i = 0; i < _selectedCellsLabel.size(); ++i)
    {
        _selectedCellsLabel[i].setText(QString("(%1 items)").arg(0));
        _selectedCellsLabel[i].setAlignment(Qt::AlignHCenter);
    }

    auto updateSelectionIndices = [this](std::vector<uint32_t>& selection, const QString& selectionName, QLabel& label) {
        if (!_points.isValid())
            return;

        selection = _points->getSelectionIndices();

        // ensure selection is unique and sorted
        auto sortAndUnique = [](std::vector<uint32_t>& selection) -> void {
            std::sort(selection.begin(), selection.end());
            const auto last = std::unique(selection.begin(), selection.end());
            selection.erase(last, selection.end());
            };

        sortAndUnique(selection);

        label.setText(QString("(%1 items)").arg(selection.size()));

        const auto otherData     = _additionalSettingsDialog.getSelectionMappingSourcePicker().getCurrentDataset<Points>();
        auto& otherDataSelection = _additionalSettingsDialog.getSelection(selectionName);
        otherDataSelection       = otherData.isValid() ? otherData->getSelection<Points>()->indices : std::vector<uint32_t>{};
        sortAndUnique(otherDataSelection);

        qDebug() << "DifferentialExpressionPlugin: Saved selection " << selectionName << " with " << selection.size() << " items.";

        if (_selectionA.size() != 0 && _selectionB.size() != 0)
            _buttonProgressBar->showStatus(TableModel::Status::OutDated);

        };

    auto highlightSelectionIndices = [this](const std::vector<uint32_t>& selection, const QString& selectionName) {
        if (!_points.isValid())
            return;

        auto otherData = _additionalSettingsDialog.getSelectionMappingSourcePicker().getCurrentDataset<Points>();

        if (otherData.isValid()) {

            // Check if the selection mapping makes sense
            const auto [selectionMapping, numPointsTarget] = getSelectionMappingOtherToCurrent(otherData, _points);
            const bool useOtherSelection = isMappingValid(selectionMapping, numPointsTarget, _points, _additionalSettingsDialog.checkMappingSurjective());

            otherData->getSelection<Points>()->indices = useOtherSelection ? _additionalSettingsDialog.getSelection(selectionName) : std::vector<uint32_t>{};
            
            events().notifyDatasetDataSelectionChanged(otherData);
        }
        else {
            _points->setSelectionIndices(selection);
            events().notifyDatasetDataSelectionChanged(_points);
        }

        };

    connect(_setSelectionTriggerActions.getTriggerAction(0), &TriggerAction::triggered, [this, updateSelectionIndices](){
            updateSelectionIndices(_selectionA, "A", _selectedCellsLabel[0]);
        });

    connect(_setSelectionTriggerActions.getTriggerAction(1), &TriggerAction::triggered, [this, updateSelectionIndices](){
            updateSelectionIndices(_selectionB, "B", _selectedCellsLabel[1]);
        });

    connect(_highlightSelectionTriggerActions.getTriggerAction(0), &TriggerAction::triggered, [this, highlightSelectionIndices](){
        highlightSelectionIndices(_selectionA, "A");
        });

    connect(_highlightSelectionTriggerActions.getTriggerAction(1), &TriggerAction::triggered, [this, highlightSelectionIndices](){
            highlightSelectionIndices(_selectionB, "B");
        });

    QGridLayout* selectionLayout = new QGridLayout();
    for (std::size_t i = 0; i < _selectedCellsLabel.size(); ++i)
    {
        selectionLayout->addWidget(_setSelectionTriggerActions.getTriggerAction(i)->createWidget(&mainWidget), 0, i);
        selectionLayout->addWidget(_highlightSelectionTriggerActions.getTriggerAction(i)->createWidget(&mainWidget), 1, i);
        selectionLayout->addWidget(&_selectedCellsLabel[i], 2, i);
    }

    layout->addLayout(selectionLayout);

     // Load points when the pointer to the position dataset changes
    connect(&_points, &Dataset<Points>::changed, this, &DifferentialExpressionPlugin::positionDatasetChanged);
    connect(&_geneComparisonDataset, &Dataset<Points>::dataSelectionChanged, this, &DifferentialExpressionPlugin::geneComparisonSelectionChanged);
}


void DifferentialExpressionPlugin::setPositionDataset(const mv::Dataset<Points>& newPoints)
{
    if (!newPoints.isValid())
    {
        qDebug() << "DifferentialExpressionPlugin Warning: invalid dataset!";
        return;
    }

    _points = newPoints;

    // Update the current dataset name label and dimension picker
    _currentDatasetNameLabel->setText(QString("Current points dataset: %1").arg(_points->getGuiName()));
    _currentSelectedDimension.setPointsDataset(_points);

    // Only show the drop indicator when nothing is loaded in the dataset reference
    _dropWidget->setShowDropIndicator(false);
}

void DifferentialExpressionPlugin::positionDatasetChanged()
{
    // Do not show the drop indicator if there is a valid point positions dataset
    _dropWidget->setShowDropIndicator(!_points.isValid());

    _tableItemModel->invalidate();
    if (!_points.isValid())
        return;

    // Compute normalization
    const auto numDimensions    = _points->getNumDimensions();
    const auto numPoints        = _points->getNumPoints();

    // check if min and max need to be recomputed or are stored
    // first check if there are dimension statistics stored in the properties and if they contain min and max values
    QVariantMap dimensionStatisticsMap = _points->getProperty("Dimension Statistics").toMap();
    bool recompute = dimensionStatisticsMap.empty();
    recompute |= (dimensionStatisticsMap.constFind("min") == dimensionStatisticsMap.constEnd());
    recompute |= (dimensionStatisticsMap.constFind("max") == dimensionStatisticsMap.constEnd());

    if (recompute)
    {
        qDebug() << "DifferentialExpressionPlugin: Computing dimension ranges";
        _minValues.resize(numDimensions, std::numeric_limits<float>::max());
        _rescaleValues.resize(numDimensions, std::numeric_limits<float>::lowest());

        std::vector<std::size_t> count(numDimensions, 0);

        local::visitAllElements(_points, [this, &count](auto row, auto column, auto value)->void
            {
                if (value > _rescaleValues[column])
                    _rescaleValues[column] = value;
                if (value < _minValues[column])
                    _minValues[column] = value;
                count[column]++;
            });

        // check for potential 0 values and add them to the min and max range if needed
#pragma omp parallel for schedule(dynamic,1)
        for (std::ptrdiff_t d = 0; d < numDimensions; d++)
        {
            if (count[d] < numPoints)
            {
                if (_minValues[d] > 0)
                    _minValues[d] = 0;
                if (_rescaleValues[d] < 0)
                    _minValues[d] = 0;
            }
        }

        // store min and max values in the properties
        dimensionStatisticsMap["min"] = QVariantList(_minValues.cbegin(), _minValues.cend());
        dimensionStatisticsMap["max"] = QVariantList(_rescaleValues.cbegin(), _rescaleValues.cend());
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
            qDebug() << "DifferentialExpressionPlugin: Loading dimension ranges";
            // load them from properties
            _minValues.resize(numDimensions);
            _rescaleValues.resize(numDimensions);
#pragma  omp parallel for
            for (std::ptrdiff_t i = 0; i < numDimensions; ++i)
            {
                _minValues[i]        = minList[i].toFloat();
                _rescaleValues[i]    = maxList[i].toFloat();
            }

        }
    }

    // Compute rescale values
#pragma omp parallel for schedule(dynamic,1)
    for (std::ptrdiff_t d = 0; d < numDimensions; d++)
    {
        const float diff = (_rescaleValues[d] - _minValues[d]);
        if (std::fabs(diff) > 1e-6f)
            _rescaleValues[d] = 1.0f / diff;
        else
            _rescaleValues[d] = 1.0f;
    }

    qDebug() << "DifferentialExpressionPlugin: Loaded " << numDimensions << " dimensions for " << numPoints << " points";
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
        qDebug() << "DifferentialExpressionPlugin: No data written to disk - File name empty";
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

    const std::ptrdiff_t numDimensions = _points->getNumDimensions();
    const size_t selectionSizeA = _selectionA.size();
    const size_t selectionSizeB = _selectionB.size();

    if (selectionSizeA == 0 || selectionSizeB == 0)
        return;

    qDebug() << "DifferentialExpressionPlugin: Computing differential expression.";

    // Determine dynamic column counts based on the toggle
    _totalTableColumns = _useAdditionalCalculations ? 10 : 6;

    // for mean, sum all values and divide by size later
    std::vector<float> meansA(numDimensions, 0);
    std::vector<float> meansB(numDimensions, 0);
    std::vector<float> geneComparisonValues(numDimensions * 2);

    // for median, collect per dimension values and sprt later
    // TODO: maybe look for median dynamically, instead of store the vectors
    std::vector<std::vector<float>> valuesA(numDimensions, std::vector<float>(selectionSizeA, 0));
    std::vector<std::vector<float>> valuesB(numDimensions, std::vector<float>(selectionSizeB, 0));
    std::vector<float> mediansA(numDimensions, 0);
    std::vector<float> mediansB(numDimensions, 0);

    // Optional calculation vectors
    // for SD
    std::vector<float> SDA(numDimensions, 0);
    std::vector<float> SDB(numDimensions, 0);

    // for percentage expressed
    std::vector<std::size_t> countExpressedA(numDimensions, 0);
    std::vector<std::size_t> countExpressedB(numDimensions, 0);
    std::vector<float> pctExpressedA(numDimensions, 0);
    std::vector<float> pctExpressedB(numDimensions, 0);

    float thr = _thresholdExpressedAction.getValue();

    // only computes extra stats if _useAdditionalCalculations is true
    auto computeAvgHelper = [this](const std::vector<uint32_t>& selectionIDs, const float thr, std::vector<float>& means, std::vector<std::vector<float>>& valCopies, std::vector<std::size_t>& countExpressed) -> void {
        local::visitElements(_points, selectionIDs, [&means, &valCopies, &countExpressed, thr, this](auto globalRowID, auto localRowID, auto column, auto value)
            {
                means[column] += value;
                valCopies[column][localRowID] = value;// for median and SD

                // count %expressed based on loaded values
                /*if (_useAdditionalCalculations && value > thr) {
                    countExpressed[column]++;
                }*/

                // count %expressed based on norm or not
                if (_useAdditionalCalculations) {
                    const float thresholdValue = _norm ? (value - _minValues[column]) * _rescaleValues[column] : value;
                    if (thresholdValue > thr) {
                        countExpressed[column]++;
                    }
                }
            });
        };

    auto computeMedian = [](std::vector<float>& vec) -> float {
        std::nth_element(vec.begin(), vec.begin() + vec.size() / 2, vec.end());
        return vec[vec.size() / 2];
        };

    // sample standard deviation
    auto computeSD = [](const std::vector<float>& vec, float mean) -> float {
        if (vec.size() < 2) 
            return 0.0f;

        float sum = 0.0f;
        for (const float& val : vec) 
        {
            const float diff = val - mean;
            sum += diff * diff;
        }
        return std::sqrt(sum / (static_cast<float>(vec.size()) - 1.0f));
        };

    auto normAvg = [&](const std::vector<float>& avgs, const std::ptrdiff_t dim) -> float {
        return (avgs[dim] - _minValues[dim]) * _rescaleValues[dim];
        };

    // first compute the sum of values per dimension for _selectionA and _selectionB
    // and copy the respective expresion values for median computation (requires sorting)
    computeAvgHelper(_selectionA, thr, meansA, valuesA, countExpressedA);
    computeAvgHelper(_selectionB, thr, meansB, valuesB, countExpressedB);

#pragma omp parallel for schedule(dynamic,1)
    for (std::ptrdiff_t d = 0; d < numDimensions; d++)
    {
        meansA[d] /= selectionSizeA;
        meansB[d] /= selectionSizeB;

        // Keep the input scale (usually log expression), before optional min-max normalization.
        geneComparisonValues[2 * d] = meansA[d];
        geneComparisonValues[2 * d + 1] = meansB[d];
        mediansA[d] = computeMedian(valuesA[d]);
        mediansB[d] = computeMedian(valuesB[d]);

        if (_useAdditionalCalculations) {
            SDA[d] = computeSD(valuesA[d], meansA[d]);
            SDB[d] = computeSD(valuesB[d], meansB[d]);
            pctExpressedA[d] = 100.0f * countExpressedA[d] / static_cast<float>(selectionSizeA);
            pctExpressedB[d] = 100.0f * countExpressedB[d] / static_cast<float>(selectionSizeB);
        }

        // Optional normalization
        if (_norm) {
            meansA[d] = normAvg(meansA, d);
            meansB[d] = normAvg(meansB, d);
            mediansA[d] = normAvg(mediansA, d);
            mediansB[d] = normAvg(mediansB, d);

            if (_useAdditionalCalculations) {
                SDA[d] = SDA[d] * _rescaleValues[d];
                SDB[d] = SDB[d] * _rescaleValues[d];
            }
        }
    }

    const auto& dimensionNames = _points->getDimensionNames();
    _tableItemModel->startModelBuilding(_totalTableColumns, numDimensions);

    _tableItemModel->setHorizontalHeader(0, QString("ID"));
    _tableItemModel->setHorizontalHeader(1, QString("DE"));
    _tableItemModel->setHorizontalHeader(2, QString("Mean (Sel. 1)"));
    _tableItemModel->setHorizontalHeader(3, QString("Mean (Sel. 2)"));
    _tableItemModel->setHorizontalHeader(4, QString("Median (Sel. 1)"));
    _tableItemModel->setHorizontalHeader(5, QString("Median (Sel. 2)"));

    if (_useAdditionalCalculations) {
        _tableItemModel->setHorizontalHeader(6, QString("SD (Sel. 1)"));
        _tableItemModel->setHorizontalHeader(7, QString("SD (Sel. 2)"));
        _tableItemModel->setHorizontalHeader(8, QString("% Expressed (Sel. 1)"));
        _tableItemModel->setHorizontalHeader(9, QString("% Expressed (Sel. 2)"));
    }

#pragma omp parallel for schedule(dynamic,1)
    for (std::ptrdiff_t dimension = 0; dimension < numDimensions; ++dimension)
    {
        std::vector<QVariant> dataVector;
        dataVector.reserve(_totalTableColumns);

        dataVector.push_back(dimensionNames[dimension]);
        dataVector.push_back(local::fround(meansA[dimension] - meansB[dimension], 3));
        dataVector.push_back(local::fround(meansA[dimension], 3));
        dataVector.push_back(local::fround(meansB[dimension], 3));
        dataVector.push_back(local::fround(mediansA[dimension], 3));
        dataVector.push_back(local::fround(mediansB[dimension], 3));

        if (_useAdditionalCalculations) {
            dataVector.push_back(local::fround(SDA[dimension], 3));
            dataVector.push_back(local::fround(SDB[dimension], 3));
            dataVector.push_back(local::fround(pctExpressedA[dimension], 3));
            dataVector.push_back(local::fround(pctExpressedB[dimension], 3));
        }

        assert(dataVector.size() == _totalTableColumns);
        _tableItemModel->setRow(dimension, dataVector, Qt::Unchecked, true);
    }
    _tableItemModel->endModelBuilding();
    updateGeneComparisonDataset(geneComparisonValues);
    updateMAPlotDataset(geneComparisonValues);
}

void DifferentialExpressionPlugin::updateGeneComparisonDataset(const std::vector<float>& values)
{
    const auto& dimensionNames = _points->getDimensionNames();
    QStringList geneNames;
    for (const auto& dimensionName : dimensionNames)
        geneNames.append(dimensionName);

    // A hierarchy parent does not share selection: these rows are genes, not cells.
    if (_geneComparisonDataset.isValid() && _geneComparisonDataset->getProperty("SourceDatasetId").toString() != _points.getDatasetId())
        _geneComparisonDataset.reset();

    if (!_geneComparisonDataset.isValid()) {
        _geneComparisonDataset = mv::data().createDataset<Points>("Points", "Gene comparison (mean 1 vs mean 2)", _points);
    }

    if (!_geneComparisonDataset.isValid())
        return;

    // A changed gene order makes previous selection indices meaningless.
    const bool geneOrderChanged = _geneComparisonDataset->getProperty("GeneNames").toStringList() != geneNames;
    if (geneOrderChanged)
        _geneComparisonDataset->setSelectionIndices({});

    _geneComparisonDataset->setData(values, 2);
    _geneComparisonDataset->setGuiName("Gene comparison (mean 1 vs mean 2)");
    _geneComparisonDataset->setDimensionNames({ "Mean 1", "Mean 2" });
    _geneComparisonDataset->setProperty("GeneNames", geneNames);
    _geneComparisonDataset->setProperty("SourceDatasetId", _points.getDatasetId());
    _geneComparisonDataset->setProperty("SelectionSize1", QVariant::fromValue<qulonglong>(_selectionA.size()));
    _geneComparisonDataset->setProperty("SelectionSize2", QVariant::fromValue<qulonglong>(_selectionB.size()));
    _geneComparisonDataset->setProperty("ExpressionScale", "Mean of input expression values; no additional transform");
    events().notifyDatasetDataChanged(_geneComparisonDataset);
    if (geneOrderChanged)
        events().notifyDatasetDataSelectionChanged(_geneComparisonDataset);
    else
        geneComparisonSelectionChanged();
}

void DifferentialExpressionPlugin::updateMAPlotDataset(const std::vector<float>& values)
{
    if (!_geneComparisonDataset.isValid() || values.size() != _geneComparisonDataset->getNumPoints() * 2)
        return;

    if (_maPlotDataset.isValid() && _maPlotDataset->getNextSourceDataset<Points>() != _geneComparisonDataset)
        _maPlotDataset.reset();

    // Both datasets have one row per gene, so share the existing gene selection.
    if (!_maPlotDataset.isValid())
        _maPlotDataset = mv::data().createDerivedDataset<Points>("Gene comparison (MA-style)", _geneComparisonDataset, _points);

    if (!_maPlotDataset.isValid())
        return;

    std::vector<float> maValues(values.size());
    for (std::size_t geneIndex = 0; geneIndex < values.size() / 2; ++geneIndex) {
        const double mean1 = values[2 * geneIndex];
        const double mean2 = values[2 * geneIndex + 1];
        // The input is already log-transformed; do not take another logarithm.
        maValues[2 * geneIndex] = static_cast<float>((mean1 + mean2) / 2.0);
        maValues[2 * geneIndex + 1] = static_cast<float>(mean1 - mean2);
    }

    _maPlotDataset->setData(maValues, 2);
    _maPlotDataset->setDimensionNames({ "A: Mean log expression", "M: Difference (1 - 2)" });
    _maPlotDataset->setProperty("GeneNames", _geneComparisonDataset->getProperty("GeneNames"));
    _maPlotDataset->setProperty("SourceDatasetId", _points.getDatasetId());
    _maPlotDataset->setProperty("SelectionSize1", _geneComparisonDataset->getProperty("SelectionSize1"));
    _maPlotDataset->setProperty("SelectionSize2", _geneComparisonDataset->getProperty("SelectionSize2"));
    _maPlotDataset->setProperty("ExpressionScale", "Log-transformed input; A = (mean 1 + mean 2) / 2; M = mean 1 - mean 2; log base unchanged");
    events().notifyDatasetDataChanged(_maPlotDataset);
}

void DifferentialExpressionPlugin::updateCurrentDimension(const std::vector<uint32_t>& dimensionIndices)
{
    if (!_points.isValid() || dimensionIndices.empty() || _updatingCurrentDimension)
        return;

    // Prevents duplicate updates
    _updatingCurrentDimension = true;

    try
    {
        auto geneIndices = dimensionIndices;
        geneIndices.erase(std::remove_if(geneIndices.begin(), geneIndices.end(), [this](uint32_t index) { return index >= _points->getNumDimensions(); }), geneIndices.end());
        std::sort(geneIndices.begin(), geneIndices.end());
        geneIndices.erase(std::unique(geneIndices.begin(), geneIndices.end()), geneIndices.end());
        if (geneIndices.empty()) {
            _updatingCurrentDimension = false;
            return;
        }

        const bool singleGene = geneIndices.size() == 1;
        const QString dimensionName = singleGene ? _points->getDimensionNames()[geneIndices.front()] : QString("Mean expression (%1 genes)").arg(static_cast<qulonglong>(geneIndices.size()));

        if (singleGene) {
            _selectedIdAction.setString(dimensionName);
            _currentSelectedDimension.setCurrentDimensionName(dimensionName);
        }

        std::vector<float> dimensionValues;
        if (singleGene) {
            _points->extractDataForDimension(dimensionValues, static_cast<int>(geneIndices.front()));

        }
        else {
            // Average the supplied values directly, including when they are already log-transformed.
            std::vector<double> expressionSums(_points->getNumPoints(), 0.0);
            std::vector<float> geneValues;
            for (const auto geneIndex : geneIndices) {
                _points->extractDataForDimension(geneValues, static_cast<int>(geneIndex));
                for (std::size_t pointIndex = 0; pointIndex < geneValues.size(); ++pointIndex)
                    expressionSums[pointIndex] += geneValues[pointIndex];
            }
            dimensionValues.resize(expressionSums.size());
            for (std::size_t pointIndex = 0; pointIndex < expressionSums.size(); ++pointIndex)
                dimensionValues[pointIndex] = static_cast<float>(expressionSums[pointIndex] / geneIndices.size());
        }

        if (_currentDimensionDataset.isValid() && _currentDimensionDataset->getNextSourceDataset<Points>() != _points)
            _currentDimensionDataset.reset();
        
        if (!_currentDimensionDataset.isValid()) {
            _currentDimensionDataset = mv::data().createDerivedDataset<Points>("CurrentDimension in DiffView", _points);
        }
        if (!_currentDimensionDataset.isValid()) {
            _updatingCurrentDimension = false;
            return;
        }

        _currentDimensionDataset->setData(dimensionValues, 1);
        _currentDimensionDataset->setDimensionNames({ dimensionName });
        events().notifyDatasetDataChanged(_currentDimensionDataset);
    }
    catch (...) // catch everything
    {
        qWarning() << "DifferentialExpressionPlugin: Failed to update current dimension";
    }

    _updatingCurrentDimension = false;
}

void DifferentialExpressionPlugin::tableView_selectionChanged(const QItemSelection&, const QItemSelection&)
{
    if (_updatingTableSelection || _tableItemModel->status() != TableModel::Status::UpToDate)
        return;

    std::vector<uint32_t> geneIndices;
    for (const auto& proxyIndex : _tableView->selectionModel()->selectedRows())
        geneIndices.push_back(static_cast<uint32_t>(_sortFilterProxyModel->mapToSource(proxyIndex).row()));
    std::sort(geneIndices.begin(), geneIndices.end());

    if (_geneComparisonDataset.isValid() && _geneComparisonDataset->getProperty("SourceDatasetId").toString() == _points.getDatasetId()) {
        // Other plugins still receive the core event; suppress only our own callback.
        const QSignalBlocker blocker(&_geneComparisonDataset);
        _geneComparisonDataset->setSelectionIndices(geneIndices);
        events().notifyDatasetDataSelectionChanged(_geneComparisonDataset);
    }

    updateCurrentDimension(geneIndices);
}

void DifferentialExpressionPlugin::geneComparisonSelectionChanged()
{
    if (!_geneComparisonDataset.isValid() || !_points.isValid() || !_tableView || _geneComparisonDataset->getProperty("SourceDatasetId").toString() != _points.getDatasetId())
        return;

    const auto geneNames = _geneComparisonDataset->getProperty("GeneNames").toStringList();
    const auto& dimensionNames = _points->getDimensionNames();
    if (geneNames.size() != static_cast<qsizetype>(dimensionNames.size()) || !std::equal(geneNames.begin(), geneNames.end(), dimensionNames.begin()))
        return;

    const auto& geneIndices = _geneComparisonDataset->getSelectionIndices();
    if (_tableItemModel->status() == TableModel::Status::UpToDate) {

        _updatingTableSelection = true;
        QItemSelection tableSelection;
        QModelIndex firstVisibleIndex;
        for (const auto geneIndex : geneIndices) {
            if (geneIndex >= dimensionNames.size())
                continue;

            const auto proxyIndex = _sortFilterProxyModel->mapFromSource(_tableItemModel->index(static_cast<int>(geneIndex), 0));
            if (!proxyIndex.isValid())
                continue; // The gene is hidden by the table filter.

            tableSelection.select(proxyIndex, proxyIndex);
            if (!firstVisibleIndex.isValid())
                firstVisibleIndex = proxyIndex;
        }
        _tableView->selectionModel()->select(tableSelection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        _tableView->selectionModel()->setCurrentIndex(firstVisibleIndex, QItemSelectionModel::NoUpdate);
        if (firstVisibleIndex.isValid()) {
            _tableView->scrollTo(firstVisibleIndex);
        }
        _updatingTableSelection = false;
    }

    updateCurrentDimension(geneIndices);
}

/******************************************************************************
 * Serialization
 ******************************************************************************/

void DifferentialExpressionPlugin::fromVariantMap(const QVariantMap& variantMap)
{
    ViewPlugin::fromVariantMap(variantMap);

    // Restore the dataset reference before actions can trigger callbacks.
    _currentDimensionDataset.reset();
    const QString currentDimensionDatasetId = variantMap.value("CurrentDimensionDatasetId").toString();
    if (!currentDimensionDatasetId.isEmpty())
        _currentDimensionDataset = mv::data().getDataset<Points>(currentDimensionDatasetId);

    _geneComparisonDataset.reset();
    const QString geneComparisonDatasetId = variantMap.value("GeneComparisonDatasetId").toString();
    if (!geneComparisonDatasetId.isEmpty())
        _geneComparisonDataset = mv::data().getDataset<Points>(geneComparisonDatasetId);

    _maPlotDataset.reset();
    const QString maPlotDatasetId = variantMap.value("MAPlotDatasetId").toString();
    if (!maPlotDatasetId.isEmpty())
        _maPlotDataset = mv::data().getDataset<Points>(maPlotDatasetId);

    for (auto action : _serializedActions)
    {
        if (variantMap.contains(action->getSerializationName()))
            action->fromParentVariantMap(variantMap);

    }

    _additionalSettingsDialog.fromParentVariantMap(variantMap);

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
    geneComparisonSelectionChanged();
}

QVariantMap DifferentialExpressionPlugin::toVariantMap() const
{
    QVariantMap variantMap = ViewPlugin::toVariantMap();

    variantMap["CurrentDimensionDatasetId"] = _currentDimensionDataset.isValid() ? _currentDimensionDataset.getDatasetId() : QString();
    variantMap["GeneComparisonDatasetId"] = _geneComparisonDataset.isValid() ? _geneComparisonDataset.getDatasetId() : QString();
    variantMap["MAPlotDatasetId"] = _maPlotDataset.isValid() ? _maPlotDataset.getDatasetId() : QString();

    for (auto action : _serializedActions)
    {
        assert(action->getSerializationName() != "#Properties");
        action->insertIntoVariantMap(variantMap);
    }

    _additionalSettingsDialog.insertIntoVariantMap(variantMap);

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

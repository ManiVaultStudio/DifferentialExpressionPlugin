#include "DifferentialExpressionPlugin.h"

#include <AbstractProjectManager.h>
#include <Application.h>
#include <DatasetsMimeData.h>
#include <PointData/storage/tiled/TiledPointsStorageBackend.h>
#include <util/BlobCodec.h>
#include <util/CodecRegistry.h>
#include <util/Serialization.h>

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QTemporaryFile>

#include "AdditionalSettings.h"
#include "WordWrapHeaderView.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>

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
    constexpr std::size_t defaultColumnBlockSize = 32;
    constexpr std::size_t medianSampleBudgetBytes = 128 * 1024 * 1024;
    constexpr std::size_t parallelSampleThreshold = 64 * 1024;

    const mv::pointdata::storage::TiledPointsStorageBackend* getTiledBackend(const Points& points)
    {
        return dynamic_cast<const mv::pointdata::storage::TiledPointsStorageBackend*>(points.getStorageBackend());
    }

    template <typename T>
    float readValue(const char* bytes)
    {
        T value;
        std::memcpy(&value, bytes, sizeof(T));
        return static_cast<float>(value);
    }

    float readValue(const char* bytes, std::uint32_t type)
    {
        using ElementType = PointData::ElementTypeSpecifier;

        switch (static_cast<ElementType>(type))
        {
            case ElementType::float32:  return readValue<float>(bytes);
            case ElementType::bfloat16: return readValue<biovault::bfloat16_t>(bytes);
            case ElementType::int32:    return readValue<std::int32_t>(bytes);
            case ElementType::uint32:   return readValue<std::uint32_t>(bytes);
            case ElementType::int16:    return readValue<std::int16_t>(bytes);
            case ElementType::uint16:   return readValue<std::uint16_t>(bytes);
            case ElementType::int8:     return readValue<std::int8_t>(bytes);
            case ElementType::uint8:    return readValue<std::uint8_t>(bytes);
        }

        throw std::runtime_error("Unsupported point-data element type");
    }

    // Keep the sample position while grouping the selection by physical row.
    using PhysicalRowSelection = std::vector<std::pair<std::uint64_t, std::size_t>>;

    template <typename Rows>
    PhysicalRowSelection mapSelectedRows(const Points& points, const Rows& rows)
    {
        const auto full = points.isFull();
        const auto rowCount = points.getNumPoints();
        PhysicalRowSelection physicalRows;
        physicalRows.reserve(rows.size());

        for (std::size_t position = 0; position < rows.size(); ++position)
        {
            const auto row = rows[position];
            if (row >= rowCount)
                throw std::out_of_range("Selected point row is out of bounds");

            const auto physicalRow = full ? row : points.indices.at(static_cast<std::size_t>(row));
            physicalRows.emplace_back(physicalRow, position);
        }

        if (!full)
            std::sort(physicalRows.begin(), physicalRows.end());

        return physicalRows;
    }

    std::size_t countSelectedTileRows(const PhysicalRowSelection& rows, std::size_t tileHeight)
    {
        std::size_t count = 0;
        auto previous = std::numeric_limits<std::uint64_t>::max();
        for (const auto& row : rows)
        {
            const auto tileRow = row.first / tileHeight;
            if (tileRow != previous)
            {
                ++count;
                previous = tileRow;
            }
        }
        return count;
    }

    class PackedTileReader
    {
    public:
        explicit PackedTileReader(const mv::pointdata::tiled::PointDataTiledStorage& storage) : _storage(storage)
        {
            if (!_storage.hasPackedTileSource())
                return;

            _codec = mv::util::codecRegistry().createCodec(nullptr, _storage.packedTileSourceCodec());
            if (!_codec)
                return;

            const auto type = _codec->getType();
            for (const auto& path : _storage.packedTileSourcePaths())
            {
                const QFileInfo file(path);
                if (file.isAbsolute() && !file.isFile())
                    return;

                // These Core codecs can resolve relative project archive entries.
                if (!file.isAbsolute() && (!mv::projects().getCurrentProject() ||
                    (type != mv::util::BlobCodec::Type::Zstd && type != mv::util::BlobCodec::Type::None)))
                    return;
            }

            _readers.resize(_storage.packedTileSourcePaths().size());
        }

        bool isAvailable() const { return !_readers.empty(); }
        std::size_t tilesRead() const { return _tilesRead; }

        QByteArray read(const mv::pointdata::tiled::TileDescriptor& descriptor)
        {
            auto& reader = _readers.at(descriptor.superTileIndex);
            if (!reader)
            {
                const auto& source = _storage.packedTileSourcePaths().at(descriptor.superTileIndex);
                auto path = source;
                if (!QFileInfo(source).isAbsolute())
                {
                    const auto stagedPath = mv::projects().resolveStagedProjectResourcePath(source);
                    if (!stagedPath.isEmpty())
                        path = stagedPath;
                }
                reader = std::make_unique<mv::util::PackedBlobFileReader>(path, _codec);
            }

            auto bytes = reader->read(descriptor.packedBlock);
            if (static_cast<std::uint64_t>(bytes.size()) != descriptor.rows * descriptor.columns * _storage.elementSize())
                throw std::runtime_error("Packed point-data tile has an unexpected size");

            ++_tilesRead;
            return bytes;
        }

    private:
        const mv::pointdata::tiled::PointDataTiledStorage& _storage;
        mv::util::SharedCodec _codec;
        std::vector<std::unique_ptr<mv::util::PackedBlobFileReader>> _readers;
        std::size_t _tilesRead = 0;
    };

    template <typename Rows, typename FunctionObject>
    void visitPackedRows(PackedTileReader& reader, const mv::pointdata::tiled::PointDataTiledStorage& storage, const Rows& rows, std::size_t firstColumn, std::size_t columnCount, FunctionObject functionObject)
    {
        const auto tileHeight = storage.rowTileSize();
        const auto tileWidth = storage.columnTileSize();
        const auto tileColumn = firstColumn / tileWidth;
        const auto tileColumns = 1 + (storage.columns() - 1) / tileWidth;
        const auto columnOffset = firstColumn % tileWidth;

        const auto end = rows.end();
        for (auto selected = rows.begin(); selected != end;)
        {
            const auto tileRow = (*selected).first / tileHeight;
            const auto& descriptor = storage.descriptors().at(tileRow * tileColumns + tileColumn);
            if (columnOffset > descriptor.columns || columnCount > descriptor.columns - columnOffset)
                throw std::out_of_range("Packed tile does not cover the selected columns");

            const auto tile = reader.read(descriptor);
            do
            {
                const auto [physicalRow, position] = *selected;
                const auto rowInTile = physicalRow % tileHeight;
                if (rowInTile >= descriptor.rows)
                    throw std::out_of_range("Packed tile does not cover the selected row");

                const auto offset = (rowInTile * descriptor.columns + columnOffset) * storage.elementSize();
                functionObject(position, tile.constData() + offset);
                ++selected;
            } while (selected != end && (*selected).first / tileHeight == tileRow);
        }
    }

    // Stage just the selected values for one physical column tile in sample order.
    void stagePackedSamples(QFile& file, PackedTileReader& reader, const mv::pointdata::tiled::PointDataTiledStorage& storage, const PhysicalRowSelection& rows, std::size_t firstColumn, std::size_t columnCount)
    {
        const auto rowBytes = static_cast<qint64>(columnCount * storage.elementSize());
        if (rows.size() > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max() / rowBytes))
            throw std::overflow_error("Differential expression samples exceed the temporary file size limit");

        visitPackedRows(reader, storage, rows, firstColumn, columnCount, [&](std::size_t position, const char* data) {
            if (!file.seek(static_cast<qint64>(position) * rowBytes) || file.write(data, rowBytes) != rowBytes)
                throw std::runtime_error("Unable to write differential expression samples to the temporary file");
        });

        if (!file.flush())
            throw std::runtime_error("Unable to flush the differential expression sample file");
    }

    template <typename FunctionObject>
    void visitStagedSamples(QFile& file, std::size_t rowCount, std::size_t rowWidth, std::size_t firstColumn, std::size_t columnCount, std::size_t elementSize, std::uint32_t type, FunctionObject functionObject)
    {
        const auto rowBytes = static_cast<qint64>(rowWidth * elementSize);
        const auto blockBytes = static_cast<qint64>(columnCount * elementSize);
        QByteArray buffer(blockBytes, Qt::Uninitialized);
        for (std::size_t row = 0; row < rowCount; ++row)
        {
            const auto offset = static_cast<qint64>(row) * rowBytes + static_cast<qint64>(firstColumn * elementSize);
            if (!file.seek(offset) || file.read(buffer.data(), blockBytes) != blockBytes)
                throw std::runtime_error("Unable to read differential expression samples from the temporary file");

            for (std::size_t column = 0; column < columnCount; ++column)
                functionObject(row, column, readValue(buffer.constData() + column * elementSize, type));
        }
    }

    template <typename Request, typename FunctionObject>
    void visitValues(std::size_t rowCount, std::size_t columnCount, Request request, FunctionObject functionObject)
    {
        std::size_t rowOffset = 0;
        std::size_t columnOffset = 0;

        request([&](const PointDataAccessView& view) {
            // A tiled read can deliver a row in several column segments.
            if (!view || view.rows > rowCount - rowOffset || view.columns > columnCount - columnOffset ||
                (view.rows > 1 && view.columns != columnCount))
                throw std::runtime_error("Unexpected point-data range shape");

            for (std::size_t row = 0; row < view.rows; ++row)
            {
                const auto* rowData = view.rowData(row);
                for (std::size_t column = 0; column < view.columns; ++column)
                {
                    const auto value = readValue(rowData + column * view.elementSizeBytes, view.elementTypeIndex);
                    functionObject(rowOffset + row, columnOffset + column, value);
                }
            }

            columnOffset += view.columns;
            if (columnOffset == columnCount)
            {
                columnOffset = 0;
                rowOffset += view.rows;
            }
        });

        if (rowOffset != rowCount || columnOffset != 0)
            throw std::runtime_error("Incomplete point-data range");
    }

    void readExpressionColumn(const Points& points, std::size_t column, std::span<float> values)
    {
        if (column >= points.getNumDimensions() || values.size() != points.getNumPoints())
            throw std::out_of_range("Expression column request does not match the dataset");

        if (values.empty())
            return;

        if (points.isProxy())
        {
            for (const auto& dataset : points.getProxyMembers())
            {
                const auto member = mv::Dataset<Points>(dataset);
                if (!member.isValid() || member->getNumPoints() > values.size())
                    throw std::runtime_error("Expression dataset proxy has an invalid member");

                const auto count = static_cast<std::size_t>(member->getNumPoints());
                readExpressionColumn(*member.get(), column, values.first(count));
                values = values.subspan(count);
            }
            if (!values.empty())
                throw std::runtime_error("Expression dataset proxy did not cover all rows");
            return;
        }

        if (const auto* backend = getTiledBackend(points))
        {
            const auto storage = backend->getStorage();
            if (storage)
            {
                PackedTileReader reader(*storage);
                if (reader.isAvailable())
                {
                    const auto logicalRows = std::views::iota(std::uint64_t{0}, points.getNumPoints());
                    const auto storeValue = [&](std::size_t row, const char* data) {
                        values[row] = readValue(data, backend->getElementTypeIndex());
                    };

                    if (points.isFull())
                    {
                        const auto physicalRows = logicalRows | std::views::transform([](std::uint64_t row) {
                            return std::pair{row, static_cast<std::size_t>(row)};
                        });
                        visitPackedRows(reader, *storage, physicalRows, column, 1, storeValue);
                    }
                    else
                        visitPackedRows(reader, *storage, mapSelectedRows(points, logicalRows), column, 1, storeValue);

                    return;
                }
            }
        }

        visitValues(values.size(), 1, [&](const PointDataAccessVisitor& visitor) {
            points.visitColumns(column, 1, visitor);
        }, [&](std::size_t row, std::size_t, float value) {
            values[row] = value;
        });
    }

    constexpr auto expressionOwnerProperty = "DifferentialExpressionPlugin.ExpressionOwner";

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
        _tableView->setSelectionMode(QAbstractItemView::SingleSelection);
        _tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        _tableView->setContextMenuPolicy(Qt::ActionsContextMenu);

        connect(_tableView, &QAbstractItemView::clicked, this, &DifferentialExpressionPlugin::tableView_clicked);
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

    auto updateSelectionIndices = [this](std::vector<uint32_t>& selection, std::vector<std::uint64_t>& rows, const QString& selectionName, QLabel& label) {
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

        if (_points->isProxy() || (_points->isFull() && !_points->isDerivedData()))
        {
            rows.assign(selection.begin(), selection.end());
        }
        else
        {
            std::vector<std::uint32_t> localRows;
            _points->getLocalSelectionIndices(localRows);
            rows.assign(localRows.begin(), localRows.end());
        }

        label.setText(QString("(%1 items)").arg(rows.size()));

        const auto otherData     = _additionalSettingsDialog.getSelectionMappingSourcePicker().getCurrentDataset<Points>();
        auto& otherDataSelection = _additionalSettingsDialog.getSelection(selectionName);
        otherDataSelection       = otherData.isValid() ? otherData->getSelection<Points>()->indices : std::vector<uint32_t>{};
        sortAndUnique(otherDataSelection);

        qDebug() << "DifferentialExpressionPlugin: Saved selection " << selectionName << " with " << selection.size() << " items.";

        if (!_selectionRowsA.empty() && !_selectionRowsB.empty())
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
            updateSelectionIndices(_selectionA, _selectionRowsA, "A", _selectedCellsLabel[0]);
        });

    connect(_setSelectionTriggerActions.getTriggerAction(1), &TriggerAction::triggered, [this, updateSelectionIndices](){
            updateSelectionIndices(_selectionB, _selectionRowsB, "B", _selectedCellsLabel[1]);
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
    _dropWidget->setShowDropIndicator(!_points.isValid());

    _selectionA.clear();
    _selectionB.clear();
    _selectionRowsA.clear();
    _selectionRowsB.clear();
    _additionalSettingsDialog.getSelectionA().clear();
    _additionalSettingsDialog.getSelectionB().clear();

    for (auto& label : _selectedCellsLabel)
        label.setText(QStringLiteral("(0 items)"));

    _tableItemModel->setStatus(TableModel::Status::Undefined);
}

void DifferentialExpressionPlugin::updateNormalization()
{
    const auto numDimensions = static_cast<std::size_t>(_points->getNumDimensions());
    const auto numPoints = _points->getNumPoints();
    auto dimensionStatistics = _points->getProperty("Dimension Statistics").toMap();
    const auto cachedMin = dimensionStatistics.value("min").toList();
    const auto cachedMax = dimensionStatistics.value("max").toList();

    if (cachedMin.size() == numDimensions && cachedMax.size() == numDimensions)
    {
        _minValues.resize(numDimensions);
        _rescaleValues.resize(numDimensions);
        for (std::size_t column = 0; column < numDimensions; ++column)
        {
            _minValues[column] = cachedMin[column].toFloat();
            _rescaleValues[column] = cachedMax[column].toFloat();
        }
    }
    else
    {
        _minValues.assign(numDimensions, std::numeric_limits<float>::max());
        _rescaleValues.assign(numDimensions, std::numeric_limits<float>::lowest());

        const auto* tiledBackend = local::getTiledBackend(*_points.get());
        const auto tiledStorage = tiledBackend ? tiledBackend->getStorage() : nullptr;
        const auto columnBlockSize = tiledStorage ? tiledStorage->columnTileSize() : local::defaultColumnBlockSize;
        const auto rowBlockSize = tiledStorage ? tiledStorage->rowTileSize() : numPoints;
        std::optional<local::PackedTileReader> packedReader;
        if (tiledStorage && _points->isFull())
            packedReader.emplace(*tiledStorage);

        for (std::uint64_t firstRow = 0; firstRow < numPoints; firstRow += rowBlockSize)
        {
            const auto rowCount = std::min<std::uint64_t>(rowBlockSize, numPoints - firstRow);

            for (std::size_t firstColumn = 0; firstColumn < numDimensions; firstColumn += columnBlockSize)
            {
                const auto columnCount = std::min(columnBlockSize, numDimensions - firstColumn);
                local::visitValues(rowCount, columnCount, [&](const PointDataAccessVisitor& visitor) {
                    if (packedReader && packedReader->isAvailable())
                    {
                        const auto tileColumns = 1 + (numDimensions - 1) / columnBlockSize;
                        const auto tileIndex = (firstRow / rowBlockSize) * tileColumns + firstColumn / columnBlockSize;
                        const auto& descriptor = tiledStorage->descriptors().at(tileIndex);
                        const auto bytes = packedReader->read(descriptor);
                        const auto elementSize = static_cast<std::uint32_t>(tiledStorage->elementSize());
                        visitor({bytes.constData(), descriptor.rows, descriptor.columns, descriptor.columns * elementSize,
                                 tiledBackend->getElementTypeIndex(), elementSize, {}});
                    }
                    else
                        _points->visitRange(firstRow, rowCount, firstColumn, columnCount, visitor);
                }, [&](std::size_t, std::size_t column, float value) {
                    const auto dimension = firstColumn + column;
                    _minValues[dimension] = std::min(_minValues[dimension], value);
                    _rescaleValues[dimension] = std::max(_rescaleValues[dimension], value);
                });
            }
        }

        dimensionStatistics["min"] = QVariantList(_minValues.cbegin(), _minValues.cend());
        dimensionStatistics["max"] = QVariantList(_rescaleValues.cbegin(), _rescaleValues.cend());
        _points->setProperty("Dimension Statistics", dimensionStatistics);
    }

    for (std::size_t column = 0; column < numDimensions; ++column)
    {
        const float diff = _rescaleValues[column] - _minValues[column];
        _rescaleValues[column] = std::fabs(diff) > 1e-6f ? 1.0f / diff : 1.0f;
    }
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

    const auto numDimensions = static_cast<std::size_t>(_points->getNumDimensions());
    const auto selectionSizeA = _selectionRowsA.size();
    const auto selectionSizeB = _selectionRowsB.size();

    if (numDimensions == 0 || selectionSizeA == 0 || selectionSizeB == 0)
        return;

    QElapsedTimer elapsed;
    elapsed.start();
    qint64 normalizationNs = 0;
    qint64 selectionReadNs = 0;

    // Determine dynamic column counts based on the toggle
    _totalTableColumns = _useAdditionalCalculations ? 10 : 6;

    std::vector<float> meansA(numDimensions, 0);
    std::vector<float> meansB(numDimensions, 0);

    std::vector<float> mediansA(numDimensions, 0);
    std::vector<float> mediansB(numDimensions, 0);

    // Optional calculation vectors
    // for SD
    std::vector<float> SDA(numDimensions, 0);
    std::vector<float> SDB(numDimensions, 0);

    // for percentage expressed
    std::vector<float> pctExpressedA(numDimensions, 0);
    std::vector<float> pctExpressedB(numDimensions, 0);

    const float thr = _thresholdExpressedAction.getValue();

    auto computeMedian = [](std::span<float> vec) -> float {
        std::nth_element(vec.begin(), vec.begin() + vec.size() / 2, vec.end());
        return vec[vec.size() / 2];
        };

    // sample standard deviation
    auto computeSD = [](std::span<const float> vec, float mean) -> float {
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

    try
    {
        if (_norm)
        {
            updateNormalization();
            normalizationNs = elapsed.nsecsElapsed();
        }

        // Read rows shared by both groups only once.
        std::vector<std::uint64_t> selectedRows;
        selectedRows.reserve(selectionSizeA + selectionSizeB);
        std::set_union(_selectionRowsA.begin(), _selectionRowsA.end(), _selectionRowsB.begin(), _selectionRowsB.end(), std::back_inserter(selectedRows));

        std::vector<std::pair<std::size_t, std::size_t>> sampleIndices;
        sampleIndices.reserve(selectedRows.size());
        std::size_t nextA = 0;
        std::size_t nextB = 0;
        for (const auto row : selectedRows)
        {
            const auto sampleA = nextA < selectionSizeA && _selectionRowsA[nextA] == row ? nextA++ : selectionSizeA;
            const auto sampleB = nextB < selectionSizeB && _selectionRowsB[nextB] == row ? nextB++ : selectionSizeB;
            sampleIndices.emplace_back(sampleA, sampleB);
        }

        const auto* tiledBackend = local::getTiledBackend(*_points.get());
        const auto tiledStorage = tiledBackend ? tiledBackend->getStorage() : nullptr;
        const auto tileWidth = tiledStorage ? tiledStorage->columnTileSize() : local::defaultColumnBlockSize;
        const bool sameSelection = _selectionRowsA == _selectionRowsB;
        const auto samplesPerColumn = sameSelection ? selectionSizeA : selectionSizeA + selectionSizeB;
        const auto maxColumnsByMemory = std::max<std::size_t>(1, local::medianSampleBudgetBytes / sizeof(float) / samplesPerColumn);
        const auto maxBlockWidth = std::min(tileWidth, maxColumnsByMemory);
        const auto physicalRows = tiledStorage ? local::mapSelectedRows(*_points.get(), selectedRows) : local::PhysicalRowSelection{};
        const auto selectedTileRows = tiledStorage ? local::countSelectedTileRows(physicalRows, tiledStorage->rowTileSize()) : 0;

        std::optional<local::PackedTileReader> packedReader;
        if (tiledStorage)
        {
            packedReader.emplace(*tiledStorage);
            if (!packedReader->isAvailable())
                packedReader.reset();
        }

        QTemporaryFile stagedSamples;
        qint64 sampleStagingNs = 0;

        const auto hitsBefore = tiledStorage ? tiledStorage->cacheHits() : 0;
        const auto missesBefore = tiledStorage ? tiledStorage->cacheMisses() : 0;
        const auto evictionsBefore = tiledStorage ? tiledStorage->cacheEvictions() : 0;
        const auto residentBytesBefore = tiledStorage ? tiledStorage->cacheSizeInBytes() : 0;
        const auto fileRangeBefore = mv::util::BlobCodec::getFileRangeTelemetry();
        std::size_t columnBlocks = 0;

        for (std::size_t firstColumn = 0; firstColumn < numDimensions;)
        {
            // Finish one physical column tile before moving to the next.
            const auto columnOffset = firstColumn % tileWidth;
            const auto tileFirstColumn = firstColumn - columnOffset;
            const auto tileColumnCount = std::min(tileWidth, numDimensions - tileFirstColumn);
            const auto columnCount = std::min(tileColumnCount - columnOffset, maxBlockWidth);
            const bool stageTile = packedReader && tileColumnCount > maxBlockWidth;
            std::vector<float> valuesA(columnCount * selectionSizeA);
            std::vector<float> valuesB(sameSelection ? 0 : columnCount * selectionSizeB);

            const auto storeSample = [&](std::size_t row, std::size_t column, float value) {
                const auto [sampleA, sampleB] = sampleIndices[row];
                if (sampleA != selectionSizeA)
                    valuesA[column * selectionSizeA + sampleA] = value;
                if (!sameSelection && sampleB != selectionSizeB)
                    valuesB[column * selectionSizeB + sampleB] = value;
            };

            const auto readStartNs = elapsed.nsecsElapsed();
            if (stageTile)
            {
                if (columnOffset == 0)
                {
                    if (!stagedSamples.isOpen())
                    {
                        const QDir tempDir(mv::Application::current()->getTemporaryDir().path());
                        stagedSamples.setFileTemplate(tempDir.filePath(QStringLiteral("de-samples-XXXXXX")));
                        if (!stagedSamples.open())
                            throw std::runtime_error("Unable to open a temporary file for differential expression samples");
                    }

                    local::stagePackedSamples(stagedSamples, *packedReader, *tiledStorage, physicalRows, tileFirstColumn, tileColumnCount);
                    sampleStagingNs += elapsed.nsecsElapsed() - readStartNs;
                }
                local::visitStagedSamples(stagedSamples, selectedRows.size(), tileColumnCount, columnOffset, columnCount, tiledStorage->elementSize(), tiledBackend->getElementTypeIndex(), storeSample);
            }
            else if (packedReader)
            {
                const auto elementSize = tiledStorage->elementSize();
                const auto type = tiledBackend->getElementTypeIndex();
                local::visitPackedRows(*packedReader, *tiledStorage, physicalRows, firstColumn, columnCount, [&](std::size_t row, const char* data) {
                    for (std::size_t column = 0; column < columnCount; ++column)
                        storeSample(row, column, local::readValue(data + column * elementSize, type));
                });
            }
            else
            {
                local::visitValues(selectedRows.size(), columnCount, [&](const PointDataAccessVisitor& visitor) {
                    _points->visitColumns(firstColumn, columnCount, selectedRows, visitor);
                }, storeSample);
            }
            selectionReadNs += elapsed.nsecsElapsed() - readStartNs;

            const bool parallel = columnCount > 1 && columnCount * samplesPerColumn >= local::parallelSampleThreshold;
#pragma omp parallel for schedule(static) if(parallel)
            for (std::ptrdiff_t column = 0; column < static_cast<std::ptrdiff_t>(columnCount); ++column)
            {
                const auto d = firstColumn + column;
                const auto samplesA = std::span(valuesA).subspan(column * selectionSizeA, selectionSizeA);
                const auto samplesB = sameSelection ? samplesA : std::span(valuesB).subspan(column * selectionSizeB, selectionSizeB);
                // Accumulate in selection order even when physical rows were reordered.
                meansA[d] = std::accumulate(samplesA.begin(), samplesA.end(), 0.0f) / selectionSizeA;
                meansB[d] = sameSelection ? meansA[d] : std::accumulate(samplesB.begin(), samplesB.end(), 0.0f) / selectionSizeB;
                mediansA[d] = computeMedian(samplesA);
                mediansB[d] = sameSelection ? mediansA[d] : computeMedian(samplesB);

                if (_useAdditionalCalculations) {
                    SDA[d] = computeSD(samplesA, meansA[d]);
                    SDB[d] = sameSelection ? SDA[d] : computeSD(samplesB, meansB[d]);
                    const auto isExpressed = [&](float value) {
                        return (_norm ? (value - _minValues[d]) * _rescaleValues[d] : value) > thr;
                    };
                    pctExpressedA[d] = 100.0f * std::count_if(samplesA.begin(), samplesA.end(), isExpressed) / static_cast<float>(selectionSizeA);
                    pctExpressedB[d] = sameSelection ? pctExpressedA[d] : 100.0f * std::count_if(samplesB.begin(), samplesB.end(), isExpressed) / static_cast<float>(selectionSizeB);
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

            firstColumn += columnCount;
            ++columnBlocks;
        }

        const auto fileRangeAfter = mv::util::BlobCodec::getFileRangeTelemetry();
        const auto backendType = _points->getStorageBackendType();
        auto diagnostics = qInfo().noquote().nospace();
        diagnostics << "DifferentialExpressionPlugin: selection I/O"
                    << " backend=" << (backendType ? mv::pointdata::storage::getStorageBackendTypeName(*backendType) : QStringLiteral("mixed-or-unknown"))
                    << " dataset-rows=" << _points->getNumPoints()
                    << " genes=" << numDimensions
                    << " a=" << selectionSizeA << " b=" << selectionSizeB << " unique-rows=" << selectedRows.size()
                    << " column-blocks=" << columnBlocks
                    << " selection-read-ms=" << selectionReadNs / 1e6
                    << " read-path=" << (packedReader ? "packed" : "points-visitor");
        if (packedReader)
            diagnostics << " source-tiles-read=" << packedReader->tilesRead() << " sample-staging-ms=" << sampleStagingNs / 1e6;

        if (tiledStorage)
        {
            const auto hitsAfter = tiledStorage->cacheHits();
            const auto missesAfter = tiledStorage->cacheMisses();
            const auto evictionsAfter = tiledStorage->cacheEvictions();
            const auto residentBytesAfter = tiledStorage->cacheSizeInBytes();
            const auto tileHeight = tiledStorage->rowTileSize();
            const auto tileColumns = 1 + (numDimensions - 1) / tileWidth;
            diagnostics << " tile-height=" << tileHeight << " tile-width=" << tileWidth
                        << " selected-tile-rows=" << selectedTileRows << " tile-columns=" << tileColumns
                        << " distinct-tiles=" << selectedTileRows * tileColumns
                        << " cache-resident-bytes-before=" << residentBytesBefore
                        << " cache-resident-bytes-after=" << residentBytesAfter;
            if (hitsAfter >= hitsBefore && missesAfter >= missesBefore && evictionsAfter >= evictionsBefore)
            {
                diagnostics << " cache-hits=" << hitsAfter - hitsBefore
                            << " cache-misses=" << missesAfter - missesBefore
                            << " cache-evictions=" << evictionsAfter - evictionsBefore;
            }
            else
                diagnostics << " cache-counters=reset";

            if (tiledStorage->hasPackedTileSource())
                diagnostics << " packed-codec=" << tiledStorage->packedTileSourceCodec();
        }
        else if (_points->isProxy())
            diagnostics << " tile-counters=unavailable-for-proxy";

        // Core's codec counters cover instrumented file-range reads across the process.
        diagnostics << " process-file-range-calls=" << fileRangeAfter.calls - fileRangeBefore.calls
                    << " process-file-range-bytes=" << fileRangeAfter.encodedBytes - fileRangeBefore.encodedBytes
                    << " process-file-read-ms=" << (fileRangeAfter.readNanoseconds - fileRangeBefore.readNanoseconds) / 1e6
                    << " process-file-decode-ms=" << (fileRangeAfter.decodeNanoseconds - fileRangeBefore.decodeNanoseconds) / 1e6;
    }
    catch (const std::exception& exception)
    {
        QMessageBox::warning(&getWidget(), tr("Differential expression"),
            tr("Unable to read point data: %1").arg(QString::fromUtf8(exception.what())));
        return;
    }

    const auto tableStartNs = elapsed.nsecsElapsed();
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
    for (std::ptrdiff_t dimension = 0; dimension < static_cast<std::ptrdiff_t>(numDimensions); ++dimension)
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

    qDebug() << "DifferentialExpressionPlugin: timings (ms)"
             << "normalization" << normalizationNs / 1e6
             << "selection reads" << selectionReadNs / 1e6
             << "remaining statistics" << (tableStartNs - normalizationNs - selectionReadNs) / 1e6
             << "table" << (elapsed.nsecsElapsed() - tableStartNs) / 1e6;
}

void DifferentialExpressionPlugin::tableView_clicked(const QModelIndex& index)
{
    if (!index.isValid() || !_points.isValid() || _tableItemModel->status() != TableModel::Status::UpToDate)
        return;

    const auto sourceIndex = _sortFilterProxyModel->mapToSource(index);
    if (!sourceIndex.isValid())
        return;

    const auto dimension = static_cast<std::size_t>(sourceIndex.row());
    const auto& dimensionNames = _points->getDimensionNames();
    if (dimension >= dimensionNames.size())
        return;

    try
    {
        const auto dimensionName = dimensionNames[dimension];
        std::vector<float> expression(_points->getNumPoints());
        local::readExpressionColumn(*_points.get(), dimension, expression);

        Dataset<Points> output;
        const auto owner = getId();
        for (const auto& child : _points->getChildren({PointType}, false))
        {
            if (!child.isValid() || child->getDataType() != PointType)
                continue;

            const auto points = Dataset<Points>(child);
            if (points->isFull() && !points->isProxy() &&
                points->getNextSourceDataset<Points>() == _points &&
                points->getProperty(local::expressionOwnerProperty).toString() == owner)
            {
                output = points;
                break;
            }
        }

        const bool created = !output.isValid();
        if (created)
        {
            output = mv::data().createDerivedDataset<Points>(QStringLiteral("Selected gene expression"), _points, _points, false);
            if (!output.isValid())
                throw std::runtime_error("Unable to create the selected gene expression dataset");

            output->setProperty(local::expressionOwnerProperty, owner);
        }

        output->setData(std::move(expression), 1);
        output->setDimensionNames({dimensionName});
        if (created)
            events().notifyDatasetAdded(output);
        events().notifyDatasetDataChanged(output);
    }
    catch (const std::exception& exception)
    {
        QMessageBox::warning(&getWidget(), tr("Selected gene expression"),
            tr("Unable to update the expression dataset: %1").arg(QString::fromUtf8(exception.what())));
    }
}

void DifferentialExpressionPlugin::tableView_selectionChanged(const QItemSelection& selected, const QItemSelection&)
{
    if (_tableItemModel->status() != TableModel::Status::UpToDate)
        return;

    const auto indexes = selected.indexes();
    if (indexes.isEmpty())
        return;

    const auto sourceIndex = _sortFilterProxyModel->mapToSource(indexes.front());
    if (!sourceIndex.isValid())
        return;

    const auto dimensionName = sourceIndex.siblingAtColumn(0).data().toString();
    _selectedIdAction.setString(dimensionName);
    _currentSelectedDimension.setCurrentDimensionIndex(sourceIndex.row());
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
}

QVariantMap DifferentialExpressionPlugin::toVariantMap() const
{
    QVariantMap variantMap = ViewPlugin::toVariantMap();

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

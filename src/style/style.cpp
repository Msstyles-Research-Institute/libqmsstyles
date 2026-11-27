#include "style.h"

#include "visualpartsmap.h"

#include "class.h"
#include "part.h"
#include "state.h"
#include "property.h"

#include <QFileInfo>
#include <QByteArrayView>
#include <QApplication>
#include <QRegularExpression>
#include <QtEndian>
#include <QtLogging>
#include <QLoggingCategory>
#include <QHash>

#include <wres/winlibrary.h>

Q_LOGGING_CATEGORY(QMSSTYLES_VISUALSTYLE_STYLE, "qmsstyles.visualstyle.style")

namespace VisualStyle
{

static const int s_propheaderSection = 4;
static const int s_propheaderSize = s_propheaderSection * 8;

struct PropertyIdentifier
{
    int classID;
    int partID;
    int stateID;
    IDENTIFIER nameID;

    friend bool operator==(const PropertyIdentifier &l, const PropertyIdentifier &r) noexcept
    {
        return l.classID == r.classID && l.partID == r.partID && l.stateID == r.stateID && l.nameID == r.nameID;
    }

    friend size_t qHash(const PropertyIdentifier &obj, size_t seed = 0) noexcept
    {
        return qHashMulti(seed, obj.classID, obj.partID, obj.stateID, obj.nameID);
    }
};

class StylePrivate
{
public:
    StylePrivate(Style *q, const QString &name, const QString &path, bool readonly)
        : q(q)
        , invalid(false)
        , name(name)
        , path(path)
        , readonly(readonly)
        , resourceTree(path.toStdString())
    {
        if (!(resourceTree.isLoaded() && resourceTree.isValid() && resourceTree.isPEBinary())) {
            qCCritical(QMSSTYLES_VISUALSTYLE_STYLE) << "Failed to load PE binary structure";
            invalid = true;
            Q_EMIT q->invalidChanged();
            return;
        }
    }

    int idForClass(QString name)
    {
        int id = -1;
        if (readonly && cachedIds.contains(name)) {
            id = cachedIds.find(name).value();
        } else {
            auto it = std::find_if(classes.begin(), classes.end(), [&](Class *cls) {
                return cls->name().compare(name, Qt::CaseInsensitive) == 0;
            });

            if (it != classes.end()) {
                id = (*it)->id();
            }

            if (readonly) {
                cachedIds.insert(name, id);
            }
        }

        return id;
    }

    Class *getClass(int classID)
    {
        if (classID >= classes.length()) {
            return nullptr;
        }

        return classes.at(classID);
    }

    Part *getPart(int classID, int partID)
    {
        Class *cls = getClass(classID);
        if (!cls) {
            return nullptr;
        }

        return cls->getPart(partID, !readonly);
    }

    State *getState(int classID, int partID, int stateID)
    {
        Part *part = getPart(classID, partID);
        if (!part) {
            return nullptr;
        }

        return part->getState(stateID, !readonly);
    }

    Property *getProperty(int classID, int partID, int stateID, IDENTIFIER nameID)
    {
        Property *property = nullptr;

        if (!readonly) {
            State *state = getState(classID, partID, stateID);
            if (!state) {
                return nullptr;
            }

            property = state->properties()->get(nameID, true);
        } else {
            PropertyIdentifier id{classID, partID, stateID, nameID};
            if (cachedProperties.contains(id)) {
                property = cachedProperties.find(id).value();
            } else {
                Class *originalCls = getClass(classID);
                if (!originalCls) {
                    return nullptr;
                }

                const Class *cls = originalCls;
                int targetPartID = partID;
                int targetStateID = stateID;

                bool stop = false;
                do {
                    Part *part = nullptr;
                    {
                        if (cls->hasPart(targetPartID)) {
                            part = cls->getPart(targetPartID, false);
                        } else if (targetPartID != 0) {
                            // look for common properties part and restart the loop
                            targetPartID = 0;
                            continue;
                        } else if (cls->baseClass()) {
                            cls = cls->baseClass();
                            targetPartID = partID;
                            continue;
                        }

                        if (!part) {
                            stop = true;
                            continue;
                        }
                    }

                    State *state = nullptr;
                    {
                        if (part->hasState(targetStateID)) {
                            state = part->getState(targetStateID, false);
                        } else if (targetStateID != 0) {
                            // look for common state and restart the loop
                            targetStateID = 0;
                            continue;
                        } else if (targetPartID != 0) {
                            targetPartID = 0;
                            targetStateID = stateID;
                            continue;
                        } else if (cls->baseClass()) {
                            cls = cls->baseClass();
                            targetPartID = partID;
                            targetStateID = stateID;
                            continue;
                        }

                        if (!state) {
                            stop = true;
                            continue;
                        }
                    }

                    property = state->properties()->get(nameID, true);
                    if (!property) {
                        if (targetStateID == 0 && targetPartID != 0) {
                            targetPartID = 0;
                            targetStateID = stateID;
                            continue;
                        } else if (targetPartID == 0 && targetStateID != 0) {
                            targetStateID = 0;
                            continue;
                        } else if (cls->baseClass()) {
                            cls = cls->baseClass();
                            targetPartID = partID;
                            targetStateID = stateID;
                            continue;
                        }
                    }

                    stop = true;
                } while (!stop);

                cachedProperties.insert(id, property);
            }
        }

        return property;
    }

    QByteArray headerlessDibToBmp(QByteArray data, QList<quint8> *pixelAlpha = nullptr) const
    {
        int headerSize = qFromLittleEndian<int>(data.sliced(0, 4).constData());

        QByteArray bmpData;
        bmpData.reserve(14 + data.size());

        QDataStream stream(&bmpData, QIODeviceBase::ReadWrite);
        stream.setByteOrder(QDataStream::LittleEndian);

        // BITMAPFILEHEADER
        stream.writeRawData("BM", 2);
        stream << quint32(14 + data.size());
        stream << quint16(0);
        stream << quint16(0);
        stream << quint32(14 + headerSize);

        bmpData.append(data);

        if (pixelAlpha) {
            int offset = 14 + headerSize;
            while (offset + 4 < bmpData.size()) {
                pixelAlpha->append(qFromLittleEndian<quint8>(bmpData.sliced(offset, sizeof(quint8))));
                offset += sizeof(quint32);
            }
        }

        return bmpData;
    }

    void loadCMAP()
    {
        wres::WinResource res = resourceTree.findResource("CMAP", "CMAP", "")->children().at(0);
        QByteArray data = QByteArray(res.offset(), res.size());

        int lastPos = 0;
        for (int i = 0; i < data.length(); i += 2) {
            if (data[i] == 0 && data[i + 1] == 0) {
                // to avoid grabbing null bytes and trying to make a name out of that
                if (i - lastPos > 2) {
                    QString cname = QString::fromLocal8Bit(data.sliced(lastPos, i - lastPos)).replace('\x00', "");
                    classes.append(new Class((int)classes.length(), cname));
                }
                lastPos = i + 2;
            }
        }

        // handle sane inheritance
        for (Class *cls : classes) {
            QStringList classStrs = cls->name().split("::");
            if (classStrs.length() > 1) {
                QString baseClass = classStrs.at(1);
                int idx = idForClass(baseClass);
                if (idx == -1) {
                    qCCritical(QMSSTYLES_VISUALSTYLE_STYLE) << cls->name() << "attempting to inherit from non-existent class" << baseClass;
                    continue;
                }

                cls->setBaseClass(classes.at(idx));
            }
        }
    }

    // handle the insane inheritance
    void loadBCMAP()
    {
        wres::WinResource res = resourceTree.findResource("BCMAP", "BCMAP", "")->children().at(0);
        QByteArray data = QByteArray(res.offset(), res.size());

        quint32 count = qFromLittleEndian<quint32>(data.sliced(0, 4).constData());
        QList<int> parents;

        for (int i = 4; i < data.length(); i += 4) {
            int index = qFromLittleEndian<quint32>(data.sliced(i, 4).constData());
            // out of range (there's tons of 0xFFFFFFFF for whatever reason which i guess means a class won't use the BCMAP?)
            if (index >= count) {
                index = -1;
            }
            parents.append(static_cast<int>(index));
        }

        for (int i = 4; i < count; i++) {
            int idx = i + 4;
            if (idx > count) {
                return;
            }
            Class *cls = classes.at(idx);
            if (i < parents.length()) {
                int parent = parents.at(i);
                if (parent > 0) {
                    cls->setBaseClass(classes.at(parent + 4));
                }
            }
        }
    }

    void structurize()
    {
        // don't bother
        // if (version == Version::WindowsLonghorn) {
        //     return;
        // }

        for (Class *cls : classes) {
            auto visualParts = VisualPartsMap::find(cls->name(), version);

            for (VisualPart &visualPart : visualParts) {
                Part *part = new Part(visualPart.id, visualPart.name);

                for (VisualState &visualState : visualPart.states) {
                    part->addState(new State(visualState.value, visualState.name));
                }

                cls->addPart(part);
            }
        }
    }

    void loadVMAP()
    {
        wres::WinResource res = resourceTree.findResource("VMAP", "VMAP", "")->children().at(0);
        QByteArray data = QByteArray(res.offset(), res.size());

        int offset = 0;
        while (offset < data.size()) {
            quint32 stringSize = qFromLittleEndian<quint32>(data.sliced(offset, 4).constData());

            int dataSize = sizeof(quint32) + stringSize * 2;
            int padding = qCeil((qreal)dataSize / 4.0) * 4 - dataSize;
            int nextOffset = offset + dataSize + padding;

            offset += sizeof(quint32);
            QString variantName;
            for (int i = 0; i < stringSize * 2; i += 2) {
                variantName.append(data.sliced(offset + i, 1).constData());
            }

            variants.append(variantName);
            offset = nextOffset;
        }
    }

    void readPropertyHeaders()
    {
        // TODO: optimize this somehow
        auto readProperties = [&](QString type, QString name) {
            wres::WinResource res = resourceTree.findResource(type.toStdString(), name.toStdString(), "")->children().at(0);
            QByteArray data(res.offset(), res.size());

            int offset = 0;
            while (offset < data.size()) {
                // see DOCUMENTATION.md for more info
                quint32 nameID     = qFromLittleEndian<quint32>(data.sliced(offset, 4).constData());
                quint32 typeID     = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection, 4).constData());
                quint32 classID    = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection * 2, 4).constData());
                quint32 partID     = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection * 3, 4).constData());
                quint32 stateID    = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection * 4, 4).constData());
                quint32 resourceId = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection * 5, 4).constData());
                quint32 reserved   = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection * 6, 4).constData());
                quint32 dataSize   = qFromLittleEndian<quint32>(data.sliced(offset + s_propheaderSection * 7, 4).constData());

                QByteArray propData{};
                // the ones that aren't 0x0 don't have any data that follows
                if (resourceId == 0x0) {
                    propData = data.sliced(offset + s_propheaderSize, dataSize);
                } else {
                    dataSize = 0;
                }

                int propertySize = s_propheaderSize + dataSize;
                int padding = qCeil((qreal)propertySize / 8.0) * 8 - propertySize;
                int nextOffset = offset + propertySize + padding;

                // check if it's valid
                {
                    if (nameID > (quint32)ATLASRECT || nameID < (quint32)DIBDATA) {
                        offset = nextOffset;
                        continue;
                    }

                    if (typeID > (quint32)ATLASRECT || typeID < (quint32)DIBDATA) {
                        offset = nextOffset;
                        continue;
                    }
                }

                Class *parentClass = getClass((qint32)classID);
                if (!parentClass) { // this should be VERY unlikely
                    parentClass = new Class((qint32)classID, "Class");
                    classes.append(parentClass);
                }

                Part *parentPart = parentClass->getPart((qint32)partID, false);
                if (!parentPart) {
                    parentPart = new Part((qint32)partID, "Part");
                    parentClass->addPart(parentPart);
                }

                State *parentState = parentPart->getState((qint32)stateID, false);
                if (!parentState) {
                    parentState = new State((qint32)stateID, "State");
                    parentPart->addState(parentState);
                }

                IDENTIFIER name = static_cast<IDENTIFIER>(nameID);
                IDENTIFIER type = static_cast<IDENTIFIER>(typeID);

                Property *property = new Property(name, type, (qint32)resourceId);
                interpretPropData(propData, resourceId, property);
                parentState->addProperty(property);

                offset = nextOffset;
            }
        };

        // documentation crap or something
        readProperties("RMAP", "RMAP");

        // element properties
        readProperties("VARIANT", variants.first().toUpper());
    }

    // TODO: and this
    void interpretPropData(QByteArray data, quint32 resourceId, Property *property)
    {
        switch (property->type())
        {
            case IDENTIFIER::INTLIST: {
                QList<quint8> list;
                quint8 count = qFromLittleEndian<quint8>(data.sliced(0, 4).constData());

                for (int i = 0; i < count; i++) {
                    list.append(qFromLittleEndian<quint32>(data.sliced(i * 4, 4).constData()));
                }

                property->setValue(list);
                break;
            }

            case IDENTIFIER::COLORLIST: {
                QList<QColor> list;

                for (int i = 0; i < data.size(); i++) {
                    quint8 r = qFromLittleEndian<quint8>(data.sliced(i, 1).constData());
                    quint8 g = qFromLittleEndian<quint8>(data.sliced(i + 1, 1).constData());
                    quint8 b = qFromLittleEndian<quint8>(data.sliced(i + 2, 1).constData());

                    list.append(QColor::fromRgb(r, g, b));
                }

                property->setValue(list);
                break;
            }

            case IDENTIFIER::STRING: {
                QString string;
                int charCount = data.size();

                for (int i = 0; i < charCount - 1; i += 2) {
                    string.append(data.sliced(i, 1).constData());
                }

                property->setValue(string);
                break;
            }

            case IDENTIFIER::FILENAME: {
                property->setValue(resourceId);

                QString groupName;
                QString fileType;
                if (version == Style::Version::WindowsLonghorn) {
                    groupName = "2"; // Bitmap
                    fileType = "BMP";
                } else {
                    groupName = "IMAGE";
                    fileType = "PNG";
                }

                wres::WinResource imageRes = resourceTree.findResource(groupName.toStdString(), std::to_string(resourceId), "")->children().at(0);
                QByteArray imageData(imageRes.offset(), imageRes.size());

                QList<quint8> pixelAlpha;
                if (version == Style::Version::WindowsLonghorn) {
                    imageData = headerlessDibToBmp(imageData, &pixelAlpha);
                }

                QImage image;
                image.loadFromData(imageData, fileType.toUtf8().constData());
                if (image.isNull()) {
                    break;
                }

                if (version == Style::Version::WindowsLonghorn) {
                    image.reinterpretAsFormat(QImage::Format_ARGB32);
                    image.flip(Qt::Vertical);

                    int i = 0;
                    for (int y = 0; y < image.height(); y++) {
                        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                        for (int x = 0; x < image.width(); x++) {
                            if (i < pixelAlpha.size()) {
                                QColor color(line[x]);
                                color.setAlpha(pixelAlpha[i++]);
                                line[x] = color.rgba();
                            }
                        }
                    }

                    image.flip(Qt::Vertical);
                    image.convertTo(QImage::Format_ARGB32_Premultiplied);
                } else {
                    image.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
                }

                property->setImageFile(QPixmap::fromImage(image));
                break;
            }

            // TODO: DISKSTREAM isn't limited to only images.
            case IDENTIFIER::DISKSTREAM: {
                property->setValue(resourceId);

                wres::WinResource streamRes = resourceTree.findResource("STREAM", std::to_string(resourceId), "")->children().at(0);
                QByteArray imageData(streamRes.offset(), streamRes.size());
                /*if (version == Version::WindowsLonghorn) {
                 *           imageData = headerlessDibToBmp(imageData);
                }*/

                QString fileType;
                if (version == Style::Version::WindowsLonghorn) {
                    fileType = "BMP";
                } else {
                    fileType = "PNG";
                }

                QImage image;
                image.loadFromData(imageData, fileType.toUtf8().constData());
                if (image.isNull()) {
                    break;
                }

                property->setImageFile(QPixmap::fromImage(image));
                break;
            }

            case IDENTIFIER::FILENAME_LITE:
            case IDENTIFIER::FONT: {
                property->setValue(qFromLittleEndian<quint8>(resourceId));
                break;
            }

            case IDENTIFIER::INT:
            case IDENTIFIER::SIZE:
            case IDENTIFIER::ENUM:
            case IDENTIFIER::HIGHCONTRASTCOLORTYPE: {
                if (resourceId == 0x0) {
                    property->setValue(qFromLittleEndian<quint32>(data.sliced(0, 4).constData()));
                } else {
                    property->setValue(0);
                }
                break;
            }

            case IDENTIFIER::BOOLTYPE: {
                if (resourceId == 0x0) {
                    property->setValue(qFromLittleEndian<bool>(data.sliced(0, 4).constData()));
                } else {
                    property->setValue(false);
                }
                break;
            }

            case IDENTIFIER::COLOR: {
                if (resourceId == 0x0) {
                    quint8 r = qFromLittleEndian<quint8>(data.sliced(0, 1).constData());
                    quint8 g = qFromLittleEndian<quint8>(data.sliced(1, 1).constData());
                    quint8 b = qFromLittleEndian<quint8>(data.sliced(2, 1).constData());

                    property->setValue(QColor::fromRgb(r, g, b));
                } else {
                    property->setValue(QColor());
                }
                break;
            }

            case IDENTIFIER::POSITION: {
                if (resourceId == 0x0) {
                    quint32 x = qFromLittleEndian<quint32>(data.sliced(0, 4).constData());
                    quint32 y = qFromLittleEndian<quint32>(data.sliced(4, 4).constData());

                    property->setValue(QPoint(x, y));
                } else {
                    property->setValue(QPoint());
                }
                break;
            }

            case IDENTIFIER::RECTTYPE: {
                if (resourceId == 0x0) {
                    quint32 x = qFromLittleEndian<quint32>(data.sliced(0, 4).constData());
                    quint32 y = qFromLittleEndian<quint32>(data.sliced(4, 4).constData());
                    quint32 w = qFromLittleEndian<quint32>(data.sliced(8, 4).constData());
                    quint32 h = qFromLittleEndian<quint32>(data.sliced(12, 4).constData());

                    property->setValue(QRect(x, y, w, h));
                } else {
                    property->setValue(QRect());
                }
                break;
            }

            case IDENTIFIER::MARGINS: {
                if (resourceId == 0x0) {
                    quint32 l = qFromLittleEndian<quint32>(data.sliced(0, 4).constData());
                    quint32 r = qFromLittleEndian<quint32>(data.sliced(4, 4).constData());
                    quint32 t = qFromLittleEndian<quint32>(data.sliced(8, 4).constData());
                    quint32 b = qFromLittleEndian<quint32>(data.sliced(12, 4).constData());

                    property->setValue(QMargins(l, t, r, b));
                } else {
                    property->setValue(QMargins());
                }
                break;
            }

            default: {
                if (resourceId == 0x0) {
                    property->setValue(data);
                } else {
                    property->setValue(QByteArray());
                }
                break;
            }

        }
    }

    // this is such a mess
    void handlePropertiesInheritance()
    {
        for (VisualStyle::Class *cls : classes) {
            for (VisualStyle::Part *part : cls->parts()) {
                for (VisualStyle::State *state : part->states()) {

                    State *common = nullptr;
                    State *stateCommon = nullptr;
                    State *baseState = nullptr;

                    if (cls->name() != "globals" && part->id() == 0 && state->id() == 0) {
                        // globals -> Common Properties
                        int globalsIdx = idForClass("globals");
                        common = getState(globalsIdx, 0, 0);
                    } else if (part->id() == 0 && state->id() != 0) {
                        // Common Properties -> (Common/State)
                        common = getState(cls->id(), 0, 0);
                    } else if (part->id() != 0 && state->id() == 0) {
                        // Common Properties -> Common
                        common = getState(cls->id(), 0, 0);
                    } else if (part->id() != 0 && state->id() != 0) {
                        // Common -> State
                        common = getState(cls->id(), part->id(), 0);
                        // (Common Properties/State) -> State
                        stateCommon = getState(cls->id(), 0, state->id());
                    }

                    if (cls->baseClass()) {
                        baseState = getState(cls->baseClass()->id(), part->id(), state->id());
                    }

                    if (common) {
                        state->properties()->setSecondFallback(common->properties());
                    }
                    if (stateCommon) {
                        state->properties()->setFirstFallback(stateCommon->properties());
                    }
                    if (baseState) {
                        state->properties()->setThirdFallback(baseState->properties());
                    }
                }
            }
        }
    }

    void saveCMAP()
    {
        QByteArray newClassMap;
        QDataStream dataStream(&newClassMap, QIODeviceBase::ReadWrite);
        dataStream.setByteOrder(QDataStream::LittleEndian);

        for (Class *cls : classes) {
            dataStream << cls->name();
            dataStream.skipRawData(4);
        }

        // TODO: inheritance
    }

    void saveProperties()
    {
        QByteArray newPropertyMap;
        QDataStream dataStream(&newPropertyMap, QIODeviceBase::ReadWrite);
        dataStream.setByteOrder(QDataStream::LittleEndian);

        for (Class *cls : classes) {
            for (Part *part : cls->parts()) {
                for (State *state : part->states()) {
                    for (Property *property : state->properties()->internalList()) {
                        dataStream << (quint32)property->name();
                        dataStream << (quint32)property->type();
                        dataStream << (quint32)cls->id();
                        dataStream << (quint32)part->id();
                        dataStream << (quint32)state->id();
                        dataStream << (quint32)property->resourceId();

                        // reserved
                        dataStream << (quint32)0x0;

                        int dataSize = 0;
                        switch (property->type())
                        {
                            case IDENTIFIER::INTLIST: {
                                QList<int> list = property->value<QList<int>>();
                                int count = list.size();

                                dataSize = count * sizeof(quint32);
                                dataStream << (quint32)dataSize;
                                for (int i = 0; i < count; i++) {
                                    dataStream << (quint32)list[i];
                                }

                                break;
                            }

                            case IDENTIFIER::COLORLIST: {
                                QList<QColor> list = property->value<QList<QColor>>();
                                int count = list.size();

                                dataSize = count * sizeof(quint8);
                                dataStream << (quint32)dataSize;
                                for (int i = 0; i < count; i++) {
                                    QColor color = list[i];
                                    dataStream << (quint8)color.red();
                                    dataStream << (quint8)color.green();
                                    dataStream << (quint8)color.blue();
                                }

                                break;
                            }

                            case IDENTIFIER::STRING: {
                                QString string = property->value<QString>();

                                dataSize = string.size() * sizeof(quint8);
                                dataStream << (quint32)dataSize;
                                dataStream << string;

                                break;
                            }

                            case IDENTIFIER::INT:
                            case IDENTIFIER::SIZE:
                            case IDENTIFIER::ENUM:
                            case IDENTIFIER::HIGHCONTRASTCOLORTYPE: {
                                dataSize = sizeof(quint32);
                                dataStream << (quint32)dataSize;
                                dataStream << (quint32)property->value<int>();
                                break;
                            }

                            case IDENTIFIER::BOOLTYPE: {
                                dataSize = sizeof(quint32);
                                dataStream << (quint32)dataSize;
                                dataStream << (quint32)property->value<bool>();
                                break;
                            }

                            case IDENTIFIER::COLOR: {
                                dataSize = sizeof(quint8) * 3;
                                dataStream << (quint32)dataSize;

                                QColor color = property->value<QColor>();
                                dataStream << (quint8)color.red();
                                dataStream << (quint8)color.green();
                                dataStream << (quint8)color.blue();

                                break;
                            }

                            case IDENTIFIER::POSITION: {
                                dataSize = sizeof(quint32) * 2;
                                dataStream << (quint32)dataSize;

                                QPoint point = property->value<QPoint>();
                                dataStream << (quint32)point.x();
                                dataStream << (quint32)point.y();

                                break;
                            }

                            case IDENTIFIER::RECTTYPE: {
                                dataSize = sizeof(quint32) * 4;
                                dataStream << (quint32)dataSize;

                                QRect rect = property->value<QRect>();
                                dataStream << (quint32)rect.x();
                                dataStream << (quint32)rect.y();
                                dataStream << (quint32)rect.width();
                                dataStream << (quint32)rect.height();

                                break;
                            }

                            case IDENTIFIER::MARGINS: {
                                dataSize = sizeof(quint32) * 4;
                                dataStream << (quint32)dataSize;

                                QMargins margins = property->value<QMargins>();
                                dataStream << (quint32)margins.left();
                                dataStream << (quint32)margins.right();
                                dataStream << (quint32)margins.top();
                                dataStream << (quint32)margins.bottom();

                                break;
                            }

                            default: {
                                dataStream << (quint32)dataSize;
                                break;
                            }
                        }

                        int propertySize = s_propheaderSize + dataSize;
                        int padding = qMin(4, qCeil((qreal)propertySize / 8.0) * 8 - propertySize);
                        dataStream.skipRawData(newPropertyMap.size() + propertySize + padding);
                    }
                }
            }
        }
    }

    // TODO: redo to not depend on existing classes perhaps
    Style::Version getVersion()
    {
        bool foundDWMTouch = false;
        bool foundDWMPen = false;
        bool foundW8Taskband = false;
        bool foundVistaQueryBuilder = false;
        bool foundTaskBand2Light_Taskband2 = false;
        bool foundWizard05_Static = false;

        for (Class *cls : classes) {
            if (cls->name() == "DWMTouch") {
                foundDWMTouch = true;
            } else if (cls->name() == "DWMPen") {
                foundDWMPen = true;
            } else if (cls->name() == "W8::TaskbandExtendedUI") {
                foundW8Taskband = true;
            } else if (cls->name() == "QueryBuilder") {
                foundVistaQueryBuilder = true;
            } else if (cls->name() == "DarkMode::TaskManager") {
                foundTaskBand2Light_Taskband2 = true;
            } else if (cls->name() == "Wizard05::Static" || cls->name() == "Wizard05Wizard::BackButton") {
                foundWizard05_Static = true;
            }
        }

        if (foundTaskBand2Light_Taskband2) {
            return Style::Version::Windows11;
        } else if (foundW8Taskband) {
            return Style::Version::Windows8;
        } else if (foundDWMTouch || foundDWMPen) {
            return Style::Version::Windows10;
        } else if (foundVistaQueryBuilder) {
            return Style::Version::WindowsVista;
        } else if (foundWizard05_Static) {
            return Style::Version::WindowsLonghorn;
        } else {
            return Style::Version::Windows7;
        }
    }

    bool invalid;

    QString name = "";
    QString path = "";
    Style::Version version = Style::Version::Unknown;

    bool readonly;
    QList<Class *> classes;
    QList<QString> variants;

    wres::WinLibrary resourceTree;

    QHash<PropertyIdentifier, Property *> cachedProperties;
    QHash<QString, int> cachedIds;

    Style *q;
};

Style::Style(const QString &name, const QString &path, const bool readonly, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<StylePrivate>(this, name, path, readonly))
{
}

Style::~Style()
{
}

bool Style::invalid()
{
    return d->invalid;
}

QString Style::name() const
{
    return d->name;
}

QString Style::path() const
{
    return d->path;
}

bool Style::isReadonly() const
{
    return d->readonly;
}

Style::Version Style::version() const
{
    return d->version;
}

const QList<Class *> &Style::classes() const
{
    return d->classes;
}

const QList<QString> &Style::variants() const
{
    return d->variants;
}

int Style::idForClass(QString name)
{
    return d->idForClass(name);
}

Class *Style::getClass(int classID)
{
    return d->getClass(classID);
}

Part *Style::getPart(int classID, int partID)
{
    return d->getPart(classID, partID);
}

State *Style::getState(int classID, int partID, int stateID)
{
    return d->getState(classID, partID, stateID);
}

Property *Style::getProperty(int classID, int partID, int stateID, IDENTIFIER nameID)
{
    return d->getProperty(classID, partID, stateID, nameID);
}

bool Style::load()
{    
    if (d->invalid) {
        qCWarning(QMSSTYLES_VISUALSTYLE_STYLE) << "Trying to load an invalid msstyles";
        return false;
    }

    d->loadCMAP();

    d->version = d->getVersion();

    d->loadBCMAP();
    if (!d->readonly) {
        d->structurize();
    }
    d->loadVMAP();
    d->readPropertyHeaders();
    // if we're faking the structure then handle inheritance normally
    if (!d->readonly) {
        d->handlePropertiesInheritance();
    }

    Q_EMIT loaded();
    return true;
}

// TODO
bool Style::save()
{
    if (d->invalid) {
        qCWarning(QMSSTYLES_VISUALSTYLE_STYLE) << "Attempting to save an invalid msstyles";
        return false;
    }

    if (d->readonly) {
        qCWarning(QMSSTYLES_VISUALSTYLE_STYLE) << "Attempted to save a read-only msstyles";
        return false;
    }

    d->saveCMAP();
    d->saveProperties();

    Q_EMIT saved();
    return true;
}

}

#include "moc_style.cpp"

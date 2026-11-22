#include "property.h"

namespace VisualStyle
{

// Property
Property::Property(IDENTIFIER name, IDENTIFIER type, int resourceId)
    : QObject(nullptr)
    , QVariant()
    , m_name(name)
    , m_type(type)
    , m_resourceId(resourceId)
{
}

IDENTIFIER Property::name() const
{
    return m_name;
}

void Property::setName(IDENTIFIER name)
{
    m_name = name;
}

IDENTIFIER Property::type() const
{
    return m_type;
}

void Property::setType(IDENTIFIER type)
{
    m_type = type;
}

QPixmap Property::imageFile() const
{
    return m_imageFile;
}

void Property::setImageFile(QPixmap imageFile)
{
    m_imageFile = imageFile;
}

int Property::resourceId() const
{
    return m_resourceId;
}

// PropertiesHandler
PropertiesHandler::PropertiesHandler(QObject *parent)
    : QObject(parent)
    , m_firstFallback(nullptr)
    , m_secondFallback(nullptr)
{
}

QList<Property *> PropertiesHandler::internalList() const
{
    return m_properties;
}

bool PropertiesHandler::contains(IDENTIFIER nameID, bool fallback)
{
    auto *prop = get(nameID, fallback);
    return !!prop;
}

Property *PropertiesHandler::get(IDENTIFIER nameID, bool fallback)
{
    auto it = std::find_if(m_properties.cbegin(), m_properties.cend(), [&](Property *property) {
        return property->name() == nameID;
    });

    if (it != m_properties.end()) {
        const Property *prop = *it;
        return m_properties.at(m_properties.indexOf(prop));
    } else if (fallback) {
        if (m_firstFallback && m_firstFallback->contains(nameID)) {
            // States set in Common Properties
            return m_firstFallback->get(nameID);
        } else if (m_secondFallback && m_secondFallback->contains(nameID)) {
            // Common Properties in the same part
            return m_secondFallback->get(nameID);
        } else if (m_thirdFallback && m_thirdFallback->contains(nameID)) {
            // State in a different class
            return m_thirdFallback->get(nameID);
        }
    }

    return nullptr;
}

void PropertiesHandler::add(Property *property)
{
    property->setParent(this);
    m_properties.append(property);
}

void PropertiesHandler::setFirstFallback(QSharedPointer<PropertiesHandler> firstFallback)
{
    Q_ASSERT(firstFallback.data() != this);
    m_firstFallback = firstFallback;
}

void PropertiesHandler::setSecondFallback(QSharedPointer<PropertiesHandler> secondFallback)
{
    Q_ASSERT(secondFallback.data() != this);
    m_secondFallback = secondFallback;
}

void PropertiesHandler::setThirdFallback(QSharedPointer<PropertiesHandler> thirdFallback)
{
    Q_ASSERT(thirdFallback.data() != this);
    m_thirdFallback = thirdFallback;
}

}

#include "moc_property.cpp"

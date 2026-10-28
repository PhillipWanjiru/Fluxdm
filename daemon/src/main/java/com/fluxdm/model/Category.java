/* FluxDM — model/Category.java */
package com.fluxdm.model;
import jakarta.persistence.*;

@Entity
@Table(name = "categories")
public class Category {
    @Id @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long   id;
    private String name;
    private String savePath;
    private String extensions;

    public Long   getId()        { return id; }
    public String getName()      { return name; }
    public void   setName(String n){ this.name = n; }
    public String getSavePath()  { return savePath; }
    public void   setSavePath(String p){ this.savePath = p; }
    public String getExtensions(){ return extensions; }
    public void   setExtensions(String e){ this.extensions = e; }
}
